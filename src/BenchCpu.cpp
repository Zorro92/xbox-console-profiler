#include "pch.h"
#include "BenchCpu.h"
#include "SystemInfo.h"

namespace {

// Latency-bound integer multiply-add dependency chain (tracks CPU clock frequency).
double RunSerialIntAlu(uint64_t iterations, uint64_t& sink_out) {
  int64_t t0 = InputState::NowQpc();
  uint64_t x = 0x123456789abcdef1ULL;
  for (uint64_t i = 0; i < iterations; ++i) {
    x = x * 0x9e3779b97f4a7c15ULL + 0xbf58476d1ce4e5b9ULL;
    x ^= (x >> 27);
  }
  int64_t t1 = InputState::NowQpc();
  sink_out = x;
  return InputState::QpcToMs(t1 - t0);
}

// Latency-bound FP64 FMA dependency chain.
double RunSerialFpAlu(uint64_t iterations, double& sink_out) {
  int64_t t0 = InputState::NowQpc();
  double a = 0.314159265358979;
  const double k = -0.9999991;
  const double c = 0.0000007;
  for (uint64_t i = 0; i < iterations; ++i) {
    a = a * k + c;
  }
  int64_t t1 = InputState::NowQpc();
  sink_out = a;
  return InputState::QpcToMs(t1 - t0);
}

struct ThreadAluResult {
  uint64_t ops = 0;
  uint64_t proc_mask = 0;
  uint64_t sink = 0;
};

// Throughput-bound 4-way independent integer+FP ALU loop run for `duration_ms`.
void AluWorker(int duration_ms, std::atomic<int>& ready, std::atomic<bool>& go,
               const std::atomic<bool>& cancel, ThreadAluResult& out) {
  uint64_t a = 0x1111111111111111ULL, b = 0x2222222222222222ULL;
  uint64_t c = 0x3333333333333333ULL, d = 0x4444444444444444ULL;
  uint64_t ops = 0;
  uint64_t mask = 0;

  ready.fetch_add(1, std::memory_order_release);
  while (!go.load(std::memory_order_acquire)) {
    YieldProcessor();
  }

  const int64_t t0 = InputState::NowQpc();
  const double target_ms = static_cast<double>(duration_ms);
  uint32_t sample_iter = 0;

  while (!cancel.load(std::memory_order_relaxed)) {
    if ((sample_iter++ & 15u) == 0) {
      PROCESSOR_NUMBER pn{};
      GetCurrentProcessorNumberEx(&pn);
      if (pn.Number < 64) {
        mask |= (1ULL << pn.Number);
      }
      if (InputState::QpcToMs(InputState::NowQpc() - t0) >= target_ms) {
        break;
      }
    }
    // 4 independent lanes x 256 iterations x 2 ops (mul + add) = 2048 ops per block.
    for (int i = 0; i < 256; ++i) {
      a = a * 0x9e3779b97f4a7c15ULL + 1ULL;
      b = b * 0xbf58476d1ce4e5b9ULL + 3ULL;
      c = c * 0x94d049bb133111ebULL + 5ULL;
      d = d * 0x2545f4914f6cdd1dULL + 7ULL;
    }
    ops += 2048;
  }

  out.ops = ops;
  out.proc_mask = mask;
  out.sink = a ^ b ^ c ^ d;
}

uint32_t PopCount64(uint64_t v) {
  uint32_t n = 0;
  while (v) {
    v &= (v - 1);
    ++n;
  }
  return n;
}

std::vector<uint32_t> BuildThreadCounts() {
  uint32_t hw = std::thread::hardware_concurrency();
  if (hw == 0) hw = 8;
  uint32_t cap = std::min<uint32_t>(std::max<uint32_t>(hw, 8), 16);
  std::vector<uint32_t> counts = {1, 2, 4, 6, 8};
  if (cap >= 12) counts.push_back(12);
  if (cap >= 16) counts.push_back(16);
  if (std::find(counts.begin(), counts.end(), hw) == counts.end() && hw <= 16) {
    counts.push_back(hw);
    std::sort(counts.begin(), counts.end());
  }
  return counts;
}

} // namespace

JsonValue RunCpuBenchmark(const std::string& state_label, const std::string& wake_mode,
                          InputState& input, const std::atomic<bool>& cancel,
                          ProfilerReport& report) {
  JsonValue j = JsonValue::Object();
  j.Set("state", JsonValue::String(state_label));
  j.Set("ms_since_last_input_start", JsonValue::Double(input.MsSinceLastInput()));

  report.Log("--- CPU BENCHMARK [" + state_label + "] (ms_since_input=" +
             std::to_string(static_cast<int>(input.MsSinceLastInput())) + ") ---");

  // 1. Single-thread serial latency ALU chains (direct clock-speed proxy).
  if (state_label == "active") input.KeepAliveIfNeeded(wake_mode, cancel);
  constexpr uint64_t kSerialIntIters = 40000000ULL;
  constexpr uint64_t kSerialFpIters = 25000000ULL;
  uint64_t int_sink = 0;
  double fp_sink = 0.0;
  // Warmup briefly
  RunSerialIntAlu(2000000ULL, int_sink);
  double int_ms = RunSerialIntAlu(kSerialIntIters, int_sink);
  double fp_ms = RunSerialFpAlu(kSerialFpIters, fp_sink);

  double int_mops = (static_cast<double>(kSerialIntIters) / (int_ms / 1000.0)) / 1e6;
  double fp_mops = (static_cast<double>(kSerialFpIters) / (fp_ms / 1000.0)) / 1e6;

  JsonValue jlat = JsonValue::Object();
  jlat.Set("int_iters", JsonValue::UInt(kSerialIntIters));
  jlat.Set("int_ms", JsonValue::Double(int_ms));
  jlat.Set("int_mchain_per_sec", JsonValue::Double(int_mops));
  jlat.Set("fp_iters", JsonValue::UInt(kSerialFpIters));
  jlat.Set("fp_ms", JsonValue::Double(fp_ms));
  jlat.Set("fp_mchain_per_sec", JsonValue::Double(fp_mops));
  jlat.Set("sink_check", JsonValue::UInt(int_sink ^ static_cast<uint64_t>(fp_sink * 1e6)));
  j.Set("serial_latency", std::move(jlat));

  {
    char buf[200];
    std::snprintf(buf, sizeof(buf),
                  "CPU serial latency: int=%.2f ms (%.1f Mchain/s) fp=%.2f ms (%.1f Mchain/s)",
                  int_ms, int_mops, fp_ms, fp_mops);
    report.Log(buf);
  }

  // 2. Per-CPU-set pinning test (verify usable cores via GetSystemCpuSetInformation + SetThreadSelectedCpuSets).
  auto sets = QueryCpuSets();
  JsonValue jpins = JsonValue::Array();
  uint32_t pin_succeeded = 0;
  uint64_t distinct_pinned_mask = 0;

  for (const auto& s : sets) {
    if (cancel.load(std::memory_order_relaxed)) break;
    if (state_label == "active") input.KeepAliveIfNeeded(wake_mode, cancel);

    bool pin_ok = false;
    uint32_t observed_proc = 0xFFFFFFFFu;
    double pin_ms = 0.0;
    uint64_t psink = 0;

    std::thread th([&]() {
      ULONG id = s.id;
      pin_ok = (SetThreadSelectedCpuSets(GetCurrentThread(), &id, 1) != FALSE);
      Sleep(1);
      PROCESSOR_NUMBER pn{};
      GetCurrentProcessorNumberEx(&pn);
      observed_proc = pn.Number;
      pin_ms = RunSerialIntAlu(8000000ULL, psink);
    });
    th.join();

    if (pin_ok) ++pin_succeeded;
    if (observed_proc < 64) distinct_pinned_mask |= (1ULL << observed_proc);
    double mchain = pin_ms > 0.0 ? (8.0 / (pin_ms / 1000.0)) : 0.0;

    JsonValue jp = JsonValue::Object();
    jp.Set("cpu_set_id", JsonValue::UInt(s.id));
    jp.Set("logical_index", JsonValue::UInt(s.logical_processor_index));
    jp.Set("core_index", JsonValue::UInt(s.core_index));
    jp.Set("allocated_to_target", JsonValue::Bool(s.allocated_to_target));
    jp.Set("pin_ok", JsonValue::Bool(pin_ok));
    jp.Set("observed_logical_proc", JsonValue::UInt(observed_proc));
    jp.Set("int_8m_ms", JsonValue::Double(pin_ms));
    jp.Set("int_mchain_per_sec", JsonValue::Double(mchain));
    jpins.Push(std::move(jp));
  }
  j.Set("cpu_set_pin_count", JsonValue::UInt(sets.size()));
  j.Set("cpu_set_pin_succeeded", JsonValue::UInt(pin_succeeded));
  j.Set("cpu_set_distinct_observed_procs", JsonValue::UInt(PopCount64(distinct_pinned_mask)));
  j.Set("cpu_set_pin_results", std::move(jpins));

  report.Log("CPU set pinning: total=" + std::to_string(sets.size()) +
             " pin_ok=" + std::to_string(pin_succeeded) +
             " distinct_observed_procs=" + std::to_string(PopCount64(distinct_pinned_mask)));

  // 3. Multi-thread ALU throughput & concurrent core occupancy across thread counts.
  JsonValue jscaling = JsonValue::Array();
  double base_gops = 0.0;
  constexpr int kDurationMs = 250;

  for (uint32_t nthreads : BuildThreadCounts()) {
    if (cancel.load(std::memory_order_relaxed)) break;
    if (state_label == "active") input.KeepAliveIfNeeded(wake_mode, cancel);

    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::vector<ThreadAluResult> results(nthreads);
    std::vector<std::thread> threads;
    threads.reserve(nthreads);

    for (uint32_t t = 0; t < nthreads; ++t) {
      threads.emplace_back(AluWorker, kDurationMs, std::ref(ready), std::ref(go),
                           std::cref(cancel), std::ref(results[t]));
    }
    while (ready.load(std::memory_order_acquire) < static_cast<int>(nthreads)) {
      YieldProcessor();
    }
    int64_t t0 = InputState::NowQpc();
    go.store(true, std::memory_order_release);
    for (auto& th : threads) th.join();
    double wall_ms = InputState::QpcToMs(InputState::NowQpc() - t0);

    uint64_t total_ops = 0;
    uint64_t union_mask = 0;
    uint64_t sink = 0;
    for (const auto& r : results) {
      total_ops += r.ops;
      union_mask |= r.proc_mask;
      sink ^= r.sink;
    }
    double gops = wall_ms > 0.0 ? (static_cast<double>(total_ops) / (wall_ms / 1000.0)) / 1e9 : 0.0;
    if (nthreads == 1) base_gops = gops;
    double scale = base_gops > 0.0 ? (gops / base_gops) : 0.0;
    uint32_t distinct = PopCount64(union_mask);

    JsonValue jprocs = JsonValue::Array();
    for (uint32_t p = 0; p < 64; ++p) {
      if (union_mask & (1ULL << p)) jprocs.Push(JsonValue::UInt(p));
    }

    JsonValue jr = JsonValue::Object();
    jr.Set("threads", JsonValue::UInt(nthreads));
    jr.Set("wall_ms", JsonValue::Double(wall_ms));
    jr.Set("total_gops_per_sec", JsonValue::Double(gops));
    jr.Set("per_thread_gops_per_sec", JsonValue::Double(gops / static_cast<double>(nthreads)));
    jr.Set("scaling_vs_1t", JsonValue::Double(scale));
    jr.Set("distinct_logical_procs", JsonValue::UInt(distinct));
    jr.Set("observed_procs", std::move(jprocs));
    jr.Set("sink_check", JsonValue::UInt(sink));
    jscaling.Push(std::move(jr));

    char buf[220];
    std::snprintf(buf, sizeof(buf),
                  "CPU ALU %2uT: %.2f Gops/s (scale %.2fx, %.2f Gops/s/T) distinct_procs=%u",
                  nthreads, gops, scale, gops / static_cast<double>(nthreads), distinct);
    report.Log(buf);
  }
  j.Set("throughput_scaling", std::move(jscaling));
  j.Set("ms_since_last_input_end", JsonValue::Double(input.MsSinceLastInput()));
  return j;
}
