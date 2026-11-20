#include "pch.h"
#include "BenchBandwidth.h"

#include <emmintrin.h>
#include <malloc.h>

namespace {

struct AlignedBuf {
  uint8_t* ptr = nullptr;
  size_t size = 0;

  explicit AlignedBuf(size_t bytes) : size(bytes) {
    ptr = static_cast<uint8_t*>(_aligned_malloc(bytes, 4096));
    if (ptr) {
      for (size_t i = 0; i < bytes; i += 4096) {
        ptr[i] = static_cast<uint8_t>((i >> 12) ^ 0x5Au);
      }
    }
  }
  ~AlignedBuf() {
    if (ptr) _aligned_free(ptr);
  }
  AlignedBuf(const AlignedBuf&) = delete;
  AlignedBuf& operator=(const AlignedBuf&) = delete;
};

void StreamCopySse2(uint8_t* dst, const uint8_t* src, size_t bytes) {
  size_t n = bytes / 64;
  auto* d = reinterpret_cast<__m128i*>(dst);
  const auto* s = reinterpret_cast<const __m128i*>(src);
  for (size_t i = 0; i < n; ++i) {
    __m128i v0 = _mm_load_si128(s + 0);
    __m128i v1 = _mm_load_si128(s + 1);
    __m128i v2 = _mm_load_si128(s + 2);
    __m128i v3 = _mm_load_si128(s + 3);
    _mm_stream_si128(d + 0, v0);
    _mm_stream_si128(d + 1, v1);
    _mm_stream_si128(d + 2, v2);
    _mm_stream_si128(d + 3, v3);
    s += 4;
    d += 4;
  }
  _mm_sfence();
}

uint64_t ReadReduceSse2(const uint8_t* src, size_t bytes) {
  size_t n = bytes / 64;
  const auto* s = reinterpret_cast<const __m128i*>(src);
  __m128i a0 = _mm_setzero_si128();
  __m128i a1 = _mm_setzero_si128();
  __m128i a2 = _mm_setzero_si128();
  __m128i a3 = _mm_setzero_si128();
  for (size_t i = 0; i < n; ++i) {
    a0 = _mm_xor_si128(a0, _mm_load_si128(s + 0));
    a1 = _mm_xor_si128(a1, _mm_load_si128(s + 1));
    a2 = _mm_xor_si128(a2, _mm_load_si128(s + 2));
    a3 = _mm_xor_si128(a3, _mm_load_si128(s + 3));
    s += 4;
  }
  __m128i sum = _mm_xor_si128(_mm_xor_si128(a0, a1), _mm_xor_si128(a2, a3));
  alignas(16) uint64_t out[2] = {};
  _mm_store_si128(reinterpret_cast<__m128i*>(out), sum);
  return out[0] ^ out[1];
}

void StreamWriteSse2(uint8_t* dst, size_t bytes, uint32_t seed) {
  size_t n = bytes / 64;
  auto* d = reinterpret_cast<__m128i*>(dst);
  __m128i v = _mm_set1_epi32(static_cast<int>(seed));
  for (size_t i = 0; i < n; ++i) {
    _mm_stream_si128(d + 0, v);
    _mm_stream_si128(d + 1, v);
    _mm_stream_si128(d + 2, v);
    _mm_stream_si128(d + 3, v);
    d += 4;
  }
  _mm_sfence();
}

std::vector<uint32_t> BuildBwThreadCounts() {
  uint32_t hw = std::thread::hardware_concurrency();
  if (hw == 0) hw = 8;
  std::vector<uint32_t> counts = {1, 2, 4, 6, 8};
  if (hw > 8 && hw <= 16) counts.push_back(hw);
  return counts;
}

double MsPer25Mb(double gbps) {
  if (gbps <= 0.0) return 0.0;
  // 24.9 MB (4K 16-bit frame reference from Bale upload probe)
  return (24.9 / 1000.0) / gbps * 1000.0;
}

} // namespace

JsonValue RunBandwidthBenchmark(const std::string& state_label, const std::string& wake_mode,
                                InputState& input, const std::atomic<bool>& cancel,
                                ProfilerReport& report) {
  JsonValue j = JsonValue::Object();
  j.Set("state", JsonValue::String(state_label));
  j.Set("ms_since_last_input_start", JsonValue::Double(input.MsSinceLastInput()));

  report.Log("--- CPU MEMORY & CACHE BANDWIDTH [" + state_label + "] (ms_since_input=" +
             std::to_string(static_cast<int>(input.MsSinceLastInput())) + ") ---");

  // 64 MiB src + 64 MiB dst exceeds L3 cache on Zen 2 (4+4 MiB) and typical desktop CPUs.
  constexpr size_t kDramBytes = 64ULL * 1024ULL * 1024ULL;
  // 128 KiB src + 128 KiB dst per thread fits comfortably in 512 KiB per-core L2 cache.
  constexpr size_t kL2BytesPerThread = 128ULL * 1024ULL;

  AlignedBuf dram_src(kDramBytes);
  AlignedBuf dram_dst(kDramBytes);
  if (!dram_src.ptr || !dram_dst.ptr) {
    j.Set("error", JsonValue::String("Failed to allocate 64 MiB DRAM buffers"));
    report.Log("BW ERROR: failed to allocate 64 MiB DRAM buffers");
    return j;
  }

  JsonValue jrows = JsonValue::Array();
  uint64_t global_sink = 0;

  for (uint32_t nthreads : BuildBwThreadCounts()) {
    if (cancel.load(std::memory_order_relaxed)) break;
    if (state_label == "active") input.KeepAliveIfNeeded(wake_mode, cancel);

    // 1. L2-resident copy (per-thread 128 KiB -> 128 KiB)
    double l2_copy_gbps = 0.0;
    {
      std::atomic<int> ready{0};
      std::atomic<bool> go{false};
      std::vector<uint64_t> copied(nthreads, 0);
      std::vector<uint64_t> sinks(nthreads, 0);
      std::vector<std::thread> threads;
      threads.reserve(nthreads);

      for (uint32_t t = 0; t < nthreads; ++t) {
        threads.emplace_back([&, t]() {
          AlignedBuf s(kL2BytesPerThread);
          AlignedBuf d(kL2BytesPerThread);
          if (s.ptr && d.ptr) {
            std::memset(s.ptr, static_cast<int>(0x31 + t), kL2BytesPerThread);
            std::memset(d.ptr, 0, kL2BytesPerThread);
            // Warm L2
            for (int w = 0; w < 8; ++w) std::memcpy(d.ptr, s.ptr, kL2BytesPerThread);
          }
          ready.fetch_add(1, std::memory_order_release);
          while (!go.load(std::memory_order_acquire)) YieldProcessor();
          if (!s.ptr || !d.ptr) return;

          int64_t t0 = InputState::NowQpc();
          uint64_t bytes_done = 0;
          while (!cancel.load(std::memory_order_relaxed)) {
            for (int r = 0; r < 64; ++r) {
              std::memcpy(d.ptr, s.ptr, kL2BytesPerThread);
            }
            bytes_done += 64ULL * kL2BytesPerThread;
            if (InputState::QpcToMs(InputState::NowQpc() - t0) >= 150.0) break;
          }
          copied[t] = bytes_done;
          sinks[t] = d.ptr[0] + d.ptr[kL2BytesPerThread - 1];
        });
      }
      while (ready.load(std::memory_order_acquire) < static_cast<int>(nthreads)) YieldProcessor();
      int64_t t0 = InputState::NowQpc();
      go.store(true, std::memory_order_release);
      for (auto& th : threads) th.join();
      double sec = InputState::QpcToMs(InputState::NowQpc() - t0) / 1000.0;
      uint64_t total_bytes = 0;
      for (uint32_t t = 0; t < nthreads; ++t) {
        total_bytes += copied[t];
        global_sink ^= sinks[t];
      }
      l2_copy_gbps = sec > 0.0 ? (static_cast<double>(total_bytes) / sec / 1e9) : 0.0;
    }

    // Helper for parallel 64 MiB DRAM passes across nthreads.
    auto run_dram_op = [&](auto&& slice_fn) -> double {
      // Align each slice to 4096 bytes.
      const size_t total_pages = kDramBytes / 4096;
      const size_t pages_per_t = total_pages / nthreads;
      // Warmup 1 pass
      slice_fn(dram_dst.ptr, dram_src.ptr, kDramBytes, 0);

      std::atomic<int> ready{0};
      std::atomic<bool> go{false};
      std::vector<uint64_t> bytes_vec(nthreads, 0);
      std::vector<std::thread> threads;
      threads.reserve(nthreads);

      for (uint32_t t = 0; t < nthreads; ++t) {
        size_t off = t * pages_per_t * 4096;
        size_t len = (t + 1 == nthreads) ? (kDramBytes - off) : (pages_per_t * 4096);
        threads.emplace_back([&, t, off, len]() {
          ready.fetch_add(1, std::memory_order_release);
          while (!go.load(std::memory_order_acquire)) YieldProcessor();
          int64_t t0 = InputState::NowQpc();
          uint64_t done = 0;
          int passes = 0;
          while (!cancel.load(std::memory_order_relaxed)) {
            slice_fn(dram_dst.ptr + off, dram_src.ptr + off, len, t + static_cast<uint32_t>(passes));
            done += len;
            ++passes;
            if (passes >= 4 && InputState::QpcToMs(InputState::NowQpc() - t0) >= 220.0) break;
          }
          bytes_vec[t] = done;
        });
      }
      while (ready.load(std::memory_order_acquire) < static_cast<int>(nthreads)) YieldProcessor();
      int64_t t0 = InputState::NowQpc();
      go.store(true, std::memory_order_release);
      for (auto& th : threads) th.join();
      double sec = InputState::QpcToMs(InputState::NowQpc() - t0) / 1000.0;
      uint64_t total_bytes = 0;
      for (uint64_t b : bytes_vec) total_bytes += b;
      return sec > 0.0 ? (static_cast<double>(total_bytes) / sec / 1e9) : 0.0;
    };

    if (state_label == "active") input.KeepAliveIfNeeded(wake_mode, cancel);
    double dram_copy_gbps = run_dram_op([](uint8_t* d, const uint8_t* s, size_t n, uint32_t) {
      std::memcpy(d, s, n);
    });

    if (state_label == "active") input.KeepAliveIfNeeded(wake_mode, cancel);
    std::atomic<uint64_t> read_sink{0};
    double dram_read_gbps = run_dram_op([&](uint8_t*, const uint8_t* s, size_t n, uint32_t) {
      read_sink.fetch_xor(ReadReduceSse2(s, n), std::memory_order_relaxed);
    });
    global_sink ^= read_sink.load(std::memory_order_relaxed);

    if (state_label == "active") input.KeepAliveIfNeeded(wake_mode, cancel);
    double dram_stream_write_gbps = run_dram_op([](uint8_t* d, const uint8_t*, size_t n, uint32_t seed) {
      StreamWriteSse2(d, n, seed);
    });

    if (state_label == "active") input.KeepAliveIfNeeded(wake_mode, cancel);
    double dram_stream_copy_gbps = run_dram_op([](uint8_t* d, const uint8_t* s, size_t n, uint32_t) {
      StreamCopySse2(d, s, n);
    });

    JsonValue jr = JsonValue::Object();
    jr.Set("threads", JsonValue::UInt(nthreads));
    jr.Set("l2_copy_gbps", JsonValue::Double(l2_copy_gbps));
    jr.Set("l2_copy_ms_per_25mb", JsonValue::Double(MsPer25Mb(l2_copy_gbps)));
    jr.Set("dram_copy_gbps", JsonValue::Double(dram_copy_gbps));
    jr.Set("dram_copy_ms_per_25mb", JsonValue::Double(MsPer25Mb(dram_copy_gbps)));
    jr.Set("dram_read_gbps", JsonValue::Double(dram_read_gbps));
    jr.Set("dram_stream_write_gbps", JsonValue::Double(dram_stream_write_gbps));
    jr.Set("dram_stream_copy_gbps", JsonValue::Double(dram_stream_copy_gbps));
    jr.Set("dram_stream_copy_ms_per_25mb", JsonValue::Double(MsPer25Mb(dram_stream_copy_gbps)));
    jrows.Push(std::move(jr));

    char buf[260];
    std::snprintf(
        buf, sizeof(buf),
        "BW %2uT: L2_copy=%.1f GB/s (%.2fms/25MB) | DRAM_copy=%.2f GB/s (%.2fms/25MB) | "
        "DRAM_read=%.2f | NT_write=%.2f | NT_copy=%.2f GB/s (%.2fms/25MB)",
        nthreads, l2_copy_gbps, MsPer25Mb(l2_copy_gbps),
        dram_copy_gbps, MsPer25Mb(dram_copy_gbps),
        dram_read_gbps, dram_stream_write_gbps,
        dram_stream_copy_gbps, MsPer25Mb(dram_stream_copy_gbps));
    report.Log(buf);
  }

  j.Set("dram_buffer_mib", JsonValue::UInt(kDramBytes / (1024 * 1024)));
  j.Set("l2_buffer_kib_per_thread", JsonValue::UInt(kL2BytesPerThread / 1024));
  j.Set("sink_check", JsonValue::UInt(global_sink));
  j.Set("results", std::move(jrows));
  j.Set("ms_since_last_input_end", JsonValue::Double(input.MsSinceLastInput()));
  return j;
}
