#include "pch.h"
#include "BenchMemory.h"
#include "SystemInfo.h"

#include <malloc.h>

namespace {

using Microsoft::WRL::ComPtr;

constexpr uint64_t kMib = 1024ULL * 1024ULL;
constexpr uint32_t kCopyMib = 64u;
constexpr uint32_t kWorkingMib = 128u;
constexpr uint64_t kSafetyMarginBytes = 32ULL * kMib;

struct CpuChunk {
  uint8_t* ptr = nullptr;
  size_t bytes = 0;

  CpuChunk() = default;
  CpuChunk(uint8_t* p, size_t b) : ptr(p), bytes(b) {}
  CpuChunk(CpuChunk&& o) noexcept : ptr(o.ptr), bytes(o.bytes) {
    o.ptr = nullptr;
    o.bytes = 0;
  }
  CpuChunk& operator=(CpuChunk&& o) noexcept {
    if (this != &o) {
      if (ptr) _aligned_free(ptr);
      ptr = o.ptr;
      bytes = o.bytes;
      o.ptr = nullptr;
      o.bytes = 0;
    }
    return *this;
  }
  ~CpuChunk() {
    if (ptr) _aligned_free(ptr);
  }
  CpuChunk(const CpuChunk&) = delete;
  CpuChunk& operator=(const CpuChunk&) = delete;
};

bool AllocAndTouchCpu(uint32_t mib, std::vector<CpuChunk>& holder) {
  size_t bytes = static_cast<size_t>(mib) * static_cast<size_t>(kMib);
  auto* p = static_cast<uint8_t*>(_aligned_malloc(bytes, 4096));
  if (!p) return false;
  // Touch every 4 KiB page with non-zero bytes so physical pages are faulted & committed.
  uint8_t seed = static_cast<uint8_t>((holder.size() * 37 + 13) & 0xFF);
  for (size_t off = 0; off < bytes; off += 4096) {
    p[off] = static_cast<uint8_t>(((off >> 12) & 0xFF) ^ seed);
  }
  holder.emplace_back(p, bytes);
  return true;
}

bool HasConsoleMemoryQuota(const MemorySnapshot& m) {
  // Real UWP Xbox quota is typically 1 GiB..16 GiB; desktop MemoryStatus reports physical RAM or huge values.
#if defined(WINAPI_FAMILY) && (WINAPI_FAMILY == WINAPI_FAMILY_APP)
  return m.memory_manager_ok && m.app_limit > 0 && m.app_limit < (1ULL << 38);
#else
  (void)m;
  return false;
#endif
}

// Compute next safe step size in MiB (or 0 if at safety margin).
uint32_t SafeNextStepMib(uint32_t requested_step, uint32_t remaining_to_max,
                         const MemorySnapshot& m, bool allow_over_limit, std::string& stop_reason) {
  uint32_t step = std::min(requested_step, remaining_to_max);
  if (step == 0) {
    stop_reason = "reached_max";
    return 0;
  }
  if (!allow_over_limit && HasConsoleMemoryQuota(m)) {
    if (m.app_usage + kSafetyMarginBytes >= m.app_limit) {
      stop_reason = "near_app_limit";
      return 0;
    }
    uint64_t avail = m.app_limit - m.app_usage - kSafetyMarginBytes;
    uint32_t avail_mib = static_cast<uint32_t>(avail / kMib);
    if (avail_mib < 16) {
      stop_reason = "near_app_limit";
      return 0;
    }
    if (step > avail_mib) {
      step = avail_mib;
    }
  }
  return step;
}

// ---------- D3D11 GPU residency & copy-timing helper ----------
class D3D11MemProbe {
 public:
  HRESULT Init(IDXGIAdapter1* adapter) {
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &fl, 1,
                                   D3D11_SDK_VERSION, &dev_, nullptr, &ctx_);
    if (FAILED(hr)) return hr;
    D3D11_QUERY_DESC qev{D3D11_QUERY_EVENT, 0};
    D3D11_QUERY_DESC qdis{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    D3D11_QUERY_DESC qts{D3D11_QUERY_TIMESTAMP, 0};
    if (FAILED(hr = dev_->CreateQuery(&qev, &q_event_))) return hr;
    if (FAILED(hr = dev_->CreateQuery(&qdis, &q_disjoint_))) return hr;
    if (FAILED(hr = dev_->CreateQuery(&qts, &q_ts0_))) return hr;
    if (FAILED(hr = dev_->CreateQuery(&qts, &q_ts1_))) return hr;
    return S_OK;
  }

  HRESULT AllocateAndTouchMib(uint32_t mib, int& out_stage) {
    uint32_t left = mib;
    while (left > 0) {
      uint32_t chunk = std::min(left, kCopyMib);
      out_stage = 1;
      D3D11_TEXTURE2D_DESC desc{};
      desc.Width = 4096;
      desc.Height = chunk * 64u; // 4096 * 64 * 4 bytes = 1 MiB exact per chunk unit
      desc.MipLevels = 1;
      desc.ArraySize = 1;
      desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      desc.SampleDesc.Count = 1;
      desc.Usage = D3D11_USAGE_DEFAULT;
      desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

      ComPtr<ID3D11Texture2D> tex;
      HRESULT hr = dev_->CreateTexture2D(&desc, nullptr, &tex);
      if (FAILED(hr)) return hr;

      ComPtr<ID3D11RenderTargetView> rtv;
      hr = dev_->CreateRenderTargetView(tex.Get(), nullptr, &rtv);
      if (FAILED(hr)) return hr;

      out_stage = 2;
      float color[4] = {0.1f + 0.01f * static_cast<float>(textures_.size() & 31), 0.3f, 0.5f, 1.0f};
      ctx_->ClearRenderTargetView(rtv.Get(), color);
      ctx_->End(q_event_.Get());
      ctx_->Flush();

      if (!WaitQuery(q_event_.Get(), nullptr, 0)) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
      textures_.push_back(tex);
      owned_mib_ += chunk;
      left -= chunk;
    }
    return S_OK;
  }

  HRESULT TimedCopy64Mib(bool warmup, double& out_ms) {
    if (textures_.size() < 2) return E_UNEXPECTED;
    if (warmup) {
      ctx_->CopyResource(textures_[1].Get(), textures_[0].Get());
      ctx_->End(q_event_.Get());
      ctx_->Flush();
      if (!WaitQuery(q_event_.Get(), nullptr, 0)) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
    ctx_->Begin(q_disjoint_.Get());
    ctx_->End(q_ts0_.Get());
    ctx_->CopyResource(textures_[1].Get(), textures_[0].Get());
    ctx_->End(q_ts1_.Get());
    ctx_->End(q_disjoint_.Get());
    ctx_->Flush();

    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dis{};
    UINT64 t0 = 0, t1 = 0;
    if (!WaitQuery(q_disjoint_.Get(), &dis, sizeof(dis))) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    if (!WaitQuery(q_ts0_.Get(), &t0, sizeof(t0))) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    if (!WaitQuery(q_ts1_.Get(), &t1, sizeof(t1))) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    if (dis.Disjoint || dis.Frequency == 0 || t1 < t0) return E_FAIL;
    out_ms = static_cast<double>(t1 - t0) * 1000.0 / static_cast<double>(dis.Frequency);
    return S_OK;
  }

  HRESULT RemovedReason() const { return dev_ ? dev_->GetDeviceRemovedReason() : S_OK; }
  uint32_t OwnedMib() const { return owned_mib_; }

 private:
  bool WaitQuery(ID3D11Asynchronous* q, void* data, UINT size) {
    int64_t t0 = InputState::NowQpc();
    while (true) {
      HRESULT hr = ctx_->GetData(q, data, size, 0);
      if (hr == S_OK) return true;
      if (hr != S_FALSE) return false;
      if (InputState::QpcToMs(InputState::NowQpc() - t0) >= 15000.0) return false;
      Sleep(1);
    }
  }

  ComPtr<ID3D11Device> dev_;
  ComPtr<ID3D11DeviceContext> ctx_;
  ComPtr<ID3D11Query> q_event_, q_disjoint_, q_ts0_, q_ts1_;
  std::vector<ComPtr<ID3D11Texture2D>> textures_;
  uint32_t owned_mib_ = 0;
};

// ---------- D3D12 GPU residency & copy-timing helper ----------
class D3D12MemProbe {
 public:
  ~D3D12MemProbe() {
    if (fence_evt_) CloseHandle(fence_evt_);
  }

  HRESULT Init(IDXGIAdapter1* adapter) {
    HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev_));
    if (FAILED(hr)) return hr;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(hr = dev_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_)))) return hr;
    if (FAILED(hr = queue_->GetTimestampFrequency(&ts_freq_)) || ts_freq_ == 0) return FAILED(hr) ? hr : E_FAIL;
    if (FAILED(hr = dev_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc_)))) return hr;
    if (FAILED(hr = dev_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc_.Get(), nullptr, IID_PPV_ARGS(&list_)))) return hr;
    list_->Close();

    if (FAILED(hr = dev_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) return hr;
    fence_evt_ = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    if (!fence_evt_) return E_OUTOFMEMORY;

    D3D12_DESCRIPTOR_HEAP_DESC rtvd{};
    rtvd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvd.NumDescriptors = 1;
    if (FAILED(hr = dev_->CreateDescriptorHeap(&rtvd, IID_PPV_ARGS(&rtv_heap_)))) return hr;

    D3D12_QUERY_HEAP_DESC qhd{};
    qhd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qhd.Count = 2;
    if (FAILED(hr = dev_->CreateQueryHeap(&qhd, IID_PPV_ARGS(&q_heap_)))) return hr;

    D3D12_HEAP_PROPERTIES rb_hp{};
    rb_hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rb_d{};
    rb_d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rb_d.Width = 2 * sizeof(uint64_t);
    rb_d.Height = 1;
    rb_d.DepthOrArraySize = 1;
    rb_d.MipLevels = 1;
    rb_d.SampleDesc.Count = 1;
    rb_d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(hr = dev_->CreateCommittedResource(&rb_hp, D3D12_HEAP_FLAG_NONE, &rb_d,
                                                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&q_rb_)))) {
      return hr;
    }
    return S_OK;
  }

  HRESULT AllocateAndTouchMib(uint32_t mib, int& out_stage) {
    uint32_t left = mib;
    while (left > 0) {
      uint32_t chunk = std::min(left, kCopyMib);
      out_stage = 1;
      D3D12_HEAP_PROPERTIES hp{};
      hp.Type = D3D12_HEAP_TYPE_DEFAULT;
      D3D12_RESOURCE_DESC desc{};
      desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      desc.Width = 4096;
      desc.Height = chunk * 64u;
      desc.DepthOrArraySize = 1;
      desc.MipLevels = 1;
      desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      desc.SampleDesc.Count = 1;
      desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

      D3D12_CLEAR_VALUE cv{};
      cv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      cv.Color[0] = 0.2f;
      cv.Color[1] = 0.4f;
      cv.Color[2] = 0.6f;
      cv.Color[3] = 1.0f;

      ComPtr<ID3D12Resource> tex;
      HRESULT hr = dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
                                                 D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&tex));
      if (FAILED(hr)) return hr;

      out_stage = 2;
      auto rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
      dev_->CreateRenderTargetView(tex.Get(), nullptr, rtv);
      if (FAILED(hr = alloc_->Reset())) return hr;
      if (FAILED(hr = list_->Reset(alloc_.Get(), nullptr))) return hr;
      list_->ClearRenderTargetView(rtv, cv.Color, 0, nullptr);
      D3D12_RESOURCE_BARRIER b{};
      b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      b.Transition.pResource = tex.Get();
      b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
      b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
      list_->ResourceBarrier(1, &b);
      if (FAILED(hr = list_->Close())) return hr;
      if (FAILED(hr = SubmitAndWait())) return hr;

      textures_.push_back(tex);
      owned_mib_ += chunk;
      left -= chunk;
    }
    return S_OK;
  }

  HRESULT TimedCopy64Mib(bool warmup, double& out_ms) {
    if (textures_.size() < 2) return E_UNEXPECTED;
    HRESULT hr = alloc_->Reset();
    if (FAILED(hr)) return hr;
    if (FAILED(hr = list_->Reset(alloc_.Get(), nullptr))) return hr;

    D3D12_RESOURCE_BARRIER pre[2]{};
    pre[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    pre[0].Transition.pResource = textures_[0].Get();
    pre[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    pre[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    pre[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;

    pre[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    pre[1].Transition.pResource = textures_[1].Get();
    pre[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    pre[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    pre[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    list_->ResourceBarrier(2, pre);

    if (warmup) {
      list_->CopyResource(textures_[1].Get(), textures_[0].Get());
    }
    list_->EndQuery(q_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
    list_->CopyResource(textures_[1].Get(), textures_[0].Get());
    list_->EndQuery(q_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
    list_->ResolveQueryData(q_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, q_rb_.Get(), 0);

    std::swap(pre[0].Transition.StateBefore, pre[0].Transition.StateAfter);
    std::swap(pre[1].Transition.StateBefore, pre[1].Transition.StateAfter);
    list_->ResourceBarrier(2, pre);

    if (FAILED(hr = list_->Close())) return hr;
    if (FAILED(hr = SubmitAndWait())) return hr;

    uint64_t* ts = nullptr;
    D3D12_RANGE rng{0, 2 * sizeof(uint64_t)};
    if (FAILED(hr = q_rb_->Map(0, &rng, reinterpret_cast<void**>(&ts)))) return hr;
    uint64_t t0 = ts[0], t1 = ts[1];
    D3D12_RANGE empty{0, 0};
    q_rb_->Unmap(0, &empty);
    if (t1 < t0) return E_FAIL;
    out_ms = static_cast<double>(t1 - t0) * 1000.0 / static_cast<double>(ts_freq_);
    return S_OK;
  }

  HRESULT RemovedReason() const { return dev_ ? dev_->GetDeviceRemovedReason() : S_OK; }
  uint32_t OwnedMib() const { return owned_mib_; }

 private:
  HRESULT SubmitAndWait() {
    ID3D12CommandList* cls[] = {list_.Get()};
    queue_->ExecuteCommandLists(1, cls);
    ++fence_val_;
    HRESULT hr = queue_->Signal(fence_.Get(), fence_val_);
    if (FAILED(hr)) return hr;
    if (fence_->GetCompletedValue() < fence_val_) {
      hr = fence_->SetEventOnCompletion(fence_val_, fence_evt_);
      if (FAILED(hr)) return hr;
      if (WaitForSingleObjectEx(fence_evt_, 15000, FALSE) != WAIT_OBJECT_0) {
        return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
      }
    }
    return S_OK;
  }

  ComPtr<ID3D12Device> dev_;
  ComPtr<ID3D12CommandQueue> queue_;
  ComPtr<ID3D12CommandAllocator> alloc_;
  ComPtr<ID3D12GraphicsCommandList> list_;
  ComPtr<ID3D12Fence> fence_;
  HANDLE fence_evt_ = nullptr;
  uint64_t fence_val_ = 0;
  UINT64 ts_freq_ = 0;
  ComPtr<ID3D12DescriptorHeap> rtv_heap_;
  ComPtr<ID3D12QueryHeap> q_heap_;
  ComPtr<ID3D12Resource> q_rb_;
  std::vector<ComPtr<ID3D12Resource>> textures_;
  uint32_t owned_mib_ = 0;
};

JsonValue RunCpuOnlyProbe(const std::string& state_label, const ProfilerConfig& cfg,
                          InputState& input, const std::atomic<bool>& cancel,
                          ProfilerReport& report) {
  JsonValue j = JsonValue::Object();
  j.Set("mode", JsonValue::String("cpu"));
  j.Set("step_mib", JsonValue::UInt(cfg.mem_step_mib));
  j.Set("max_mib", JsonValue::UInt(cfg.mem_max_mib));

  report.Log("--- MEMORY PROBE: cpu [" + state_label + "] step=" + std::to_string(cfg.mem_step_mib) +
             " max=" + std::to_string(cfg.mem_max_mib) + " ---");

  std::vector<CpuChunk> chunks;
  uint32_t owned_mib = 0;
  std::string stop_reason = "completed";
  JsonValue jsteps = JsonValue::Array();

  // Baseline row (0 MiB)
  {
    MemorySnapshot snap = CaptureMemorySnapshot();
    JsonValue js = JsonValue::Object();
    js.Set("cpu_mib", JsonValue::UInt(0));
    js.Set("step_ms", JsonValue::Double(0.0));
    js.Set("ms_since_last_input", JsonValue::Double(input.MsSinceLastInput()));
    js.Set("memory", MemorySnapshotToJson(snap));
    jsteps.Push(std::move(js));
    report.Log("MEM[cpu] cpu=0 MiB | " + FormatMemoryLine(snap));
  }

  while (!cancel.load(std::memory_order_relaxed)) {
    if (state_label == "active") input.KeepAliveIfNeeded(cfg.wake_mode, cancel);
    MemorySnapshot before = CaptureMemorySnapshot();
    uint32_t next = SafeNextStepMib(cfg.mem_step_mib,
                                    owned_mib < cfg.mem_max_mib ? cfg.mem_max_mib - owned_mib : 0,
                                    before, cfg.allow_over_limit, stop_reason);
    if (next == 0) break;

    int64_t t0 = InputState::NowQpc();
    bool ok = AllocAndTouchCpu(next, chunks);
    double elapsed_ms = InputState::QpcToMs(InputState::NowQpc() - t0);
    MemorySnapshot after = CaptureMemorySnapshot();
    if (ok) owned_mib += next;

    JsonValue js = JsonValue::Object();
    js.Set("cpu_mib", JsonValue::UInt(owned_mib));
    js.Set("step_mib", JsonValue::UInt(next));
    js.Set("ok", JsonValue::Bool(ok));
    js.Set("step_ms", JsonValue::Double(elapsed_ms));
    js.Set("ms_since_last_input", JsonValue::Double(input.MsSinceLastInput()));
    js.Set("memory", MemorySnapshotToJson(after));
    jsteps.Push(std::move(js));

    char buf[220];
    std::snprintf(buf, sizeof(buf), "MEM[cpu] cpu=%u MiB ok=%d (%.1f ms) | %s",
                  owned_mib, ok ? 1 : 0, elapsed_ms, FormatMemoryLine(after).c_str());
    report.Log(buf);

    if (!ok) {
      stop_reason = "alloc_failed";
      break;
    }
  }
  if (cancel.load(std::memory_order_relaxed)) stop_reason = "cancelled";

  uint32_t peak_cpu = owned_mib;
  chunks.clear();
  Sleep(250);
  MemorySnapshot freed = CaptureMemorySnapshot();
  report.Log("MEM[cpu] STOP reason=" + stop_reason + " peak_cpu=" + std::to_string(peak_cpu) +
             " MiB | freed: " + FormatMemoryLine(freed));

  j.Set("peak_cpu_mib", JsonValue::UInt(peak_cpu));
  j.Set("stop_reason", JsonValue::String(stop_reason));
  j.Set("steps", std::move(jsteps));
  j.Set("freed_memory", MemorySnapshotToJson(freed));
  return j;
}

template <typename GpuProbeT>
JsonValue RunGpuOrMixedProbe(const std::string& mode_name, uint32_t hold_cpu_mib,
                             const std::string& state_label, const ProfilerConfig& cfg,
                             InputState& input, const std::atomic<bool>& cancel,
                             ProfilerReport& report) {
  JsonValue j = JsonValue::Object();
  j.Set("mode", JsonValue::String(mode_name));
  j.Set("step_mib", JsonValue::UInt(cfg.mem_step_mib));
  j.Set("max_mib", JsonValue::UInt(cfg.mem_max_mib));
  j.Set("requested_cpu_hold_mib", JsonValue::UInt(hold_cpu_mib));

  report.Log("--- MEMORY PROBE: " + mode_name + " [" + state_label + "] step=" +
             std::to_string(cfg.mem_step_mib) + " max=" + std::to_string(cfg.mem_max_mib) +
             " cpu_hold=" + std::to_string(hold_cpu_mib) + " ---");

  std::vector<CpuChunk> cpu_hold;
  uint32_t held_cpu = 0;
  std::string stop_reason = "completed";
  JsonValue jsteps = JsonValue::Array();

  // Hold CPU memory first for mixed mode
  if (hold_cpu_mib > 0) {
    uint32_t left = hold_cpu_mib;
    while (left > 0 && !cancel.load(std::memory_order_relaxed)) {
      uint32_t chunk = std::min(left, cfg.mem_step_mib);
      if (!AllocAndTouchCpu(chunk, cpu_hold)) {
        stop_reason = "cpu_hold_alloc_failed";
        break;
      }
      held_cpu += chunk;
      left -= chunk;
    }
    MemorySnapshot after_cpu = CaptureMemorySnapshot();
    report.Log("MEM[" + mode_name + "] held cpu=" + std::to_string(held_cpu) + " MiB | " +
               FormatMemoryLine(after_cpu));
    j.Set("after_cpu_hold_memory", MemorySnapshotToJson(after_cpu));
    if (held_cpu < hold_cpu_mib) {
      cpu_hold.clear();
      Sleep(200);
      j.Set("stop_reason", JsonValue::String(stop_reason));
      j.Set("freed_memory", MemorySnapshotToJson(CaptureMemorySnapshot()));
      return j;
    }
  }

  ComPtr<IDXGIFactory4> factory;
  ComPtr<IDXGIAdapter1> adapter;
  ComPtr<IDXGIAdapter3> adapter3;
  std::string err;
  if (!CreateAdapter(cfg.use_warp, factory, adapter, err)) {
    j.Set("stop_reason", JsonValue::String("adapter_error: " + err));
    cpu_hold.clear();
    return j;
  }
  adapter.As(&adapter3);

  uint32_t peak_gpu = 0;
  double baseline_copy_ms = 0.0;
  {
    GpuProbeT probe;
    HRESULT hr = probe.Init(adapter.Get());
    if (FAILED(hr)) {
      stop_reason = "device_init_failed_" + FormatHr(hr);
      report.Log("MEM[" + mode_name + "] STOP " + stop_reason);
      j.Set("stop_reason", JsonValue::String(stop_reason));
      cpu_hold.clear();
      return j;
    }

    bool first_step = true;
    while (!cancel.load(std::memory_order_relaxed)) {
      if (state_label == "active") input.KeepAliveIfNeeded(cfg.wake_mode, cancel);
      MemorySnapshot before = CaptureMemorySnapshot();

      uint32_t add_mib = 0;
      if (first_step) {
        if (cfg.mem_max_mib < kWorkingMib) {
          stop_reason = "max_below_working_pair";
          break;
        }
        // Check safety before allocating the 128 MiB working pair
        std::string pre_stop;
        if (!cfg.allow_over_limit && HasConsoleMemoryQuota(before) &&
            before.app_usage + kWorkingMib * kMib + kSafetyMarginBytes >= before.app_limit) {
          stop_reason = "near_app_limit_before_working_pair";
          break;
        }
        add_mib = kWorkingMib;
      } else {
        uint32_t rem_max = probe.OwnedMib() < cfg.mem_max_mib ? (cfg.mem_max_mib - probe.OwnedMib()) : 0;
        add_mib = SafeNextStepMib(cfg.mem_step_mib, rem_max, before, cfg.allow_over_limit, stop_reason);
        if (add_mib == 0) break;
      }

      int stage = 0;
      int64_t t0 = InputState::NowQpc();
      HRESULT step_hr = probe.AllocateAndTouchMib(add_mib, stage);
      double copy_ms = 0.0;
      if (SUCCEEDED(step_hr)) {
        stage = 3;
        step_hr = probe.TimedCopy64Mib(first_step, copy_ms);
      }
      double elapsed_ms = InputState::QpcToMs(InputState::NowQpc() - t0);
      HRESULT rem_hr = probe.RemovedReason();

      MemorySnapshot after = CaptureMemorySnapshot();
      DxgiMemorySnapshot dxgi = QueryDxgiVideoMemory(adapter3.Get());
      peak_gpu = probe.OwnedMib();

      if (first_step && SUCCEEDED(step_hr) && copy_ms > 0.0) {
        baseline_copy_ms = copy_ms;
      }
      double copy_ratio = (baseline_copy_ms > 0.0 && copy_ms > 0.0) ? (copy_ms / baseline_copy_ms) : 0.0;

      JsonValue js = JsonValue::Object();
      js.Set("cpu_mib", JsonValue::UInt(held_cpu));
      js.Set("gpu_mib", JsonValue::UInt(probe.OwnedMib()));
      js.Set("added_mib", JsonValue::UInt(add_mib));
      js.Set("stage", JsonValue::Int(stage));
      js.Set("hr", JsonValue::String(FormatHr(step_hr)));
      js.Set("device_removed_hr", JsonValue::String(FormatHr(rem_hr)));
      js.Set("copy_ms", JsonValue::Double(copy_ms));
      js.Set("copy_ratio", JsonValue::Double(copy_ratio));
      js.Set("step_ms", JsonValue::Double(elapsed_ms));
      js.Set("ms_since_last_input", JsonValue::Double(input.MsSinceLastInput()));
      js.Set("memory", MemorySnapshotToJson(after));
      js.Set("dxgi", DxgiMemoryToJson(dxgi));
      jsteps.Push(std::move(js));

      char buf[512];
      std::snprintf(buf, sizeof(buf),
                    "MEM[%s] cpu=%u gpu=%u MiB copy=%.3f ms (%.2fx) stg=%d hr=%s rem=%s | %s | %s",
                    mode_name.c_str(), held_cpu, probe.OwnedMib(), copy_ms, copy_ratio, stage,
                    FormatHr(step_hr).c_str(), FormatHr(rem_hr).c_str(),
                    FormatMemoryLine(after).c_str(), FormatDxgiLine(dxgi).c_str());
      report.Log(buf);

      first_step = false;

      if (FAILED(rem_hr)) {
        stop_reason = "device_removed_" + FormatHr(rem_hr);
        break;
      }
      if (FAILED(step_hr)) {
        stop_reason = "step_failed_stage" + std::to_string(stage) + "_" + FormatHr(step_hr);
        break;
      }
      if (baseline_copy_ms > 0.0 && copy_ms > baseline_copy_ms * 5.0) {
        stop_reason = "copy_slowdown_gt_5x_paging";
        break;
      }
    }
    if (cancel.load(std::memory_order_relaxed)) stop_reason = "cancelled";
  }

  cpu_hold.clear();
  Sleep(300);
  MemorySnapshot freed = CaptureMemorySnapshot();
  DxgiMemorySnapshot freed_dxgi = QueryDxgiVideoMemory(adapter3.Get());

  report.Log("MEM[" + mode_name + "] STOP reason=" + stop_reason + " peak_gpu=" +
             std::to_string(peak_gpu) + " MiB base_copy=" + std::to_string(baseline_copy_ms) +
             " ms | freed: " + FormatMemoryLine(freed) + " | " + FormatDxgiLine(freed_dxgi));

  j.Set("held_cpu_mib", JsonValue::UInt(held_cpu));
  j.Set("peak_gpu_mib", JsonValue::UInt(peak_gpu));
  j.Set("baseline_copy_ms", JsonValue::Double(baseline_copy_ms));
  j.Set("stop_reason", JsonValue::String(stop_reason));
  j.Set("steps", std::move(jsteps));
  j.Set("freed_memory", MemorySnapshotToJson(freed));
  j.Set("freed_dxgi", DxgiMemoryToJson(freed_dxgi));
  return j;
}

uint32_t ChooseMixedCpuMib(const ProfilerConfig& cfg) {
  if (cfg.mixed_cpu_mib > 0) return cfg.mixed_cpu_mib;
  if (cfg.use_warp) return 100u;
  MemorySnapshot m = CaptureMemorySnapshot();
  if (HasConsoleMemoryQuota(m)) {
    uint64_t limit_mib = m.app_limit / kMib;
    uint32_t forty_pct = static_cast<uint32_t>((limit_mib * 40ULL) / 100ULL);
    // Round to 100 MiB, clamped to [200, 1000]
    uint32_t rounded = (forty_pct / 100u) * 100u;
    if (rounded < 200u) rounded = 200u;
    if (rounded > 1000u) rounded = 1000u;
    return rounded;
  }
  return 1000u;
}

} // namespace

JsonValue RunMemorySuite(const std::string& state_label, const ProfilerConfig& cfg,
                         InputState& input, const std::atomic<bool>& cancel,
                         ProfilerReport& report) {
  JsonValue j = JsonValue::Object();
  j.Set("state", JsonValue::String(state_label));
  j.Set("ms_since_last_input_start", JsonValue::Double(input.MsSinceLastInput()));

  uint32_t mixed_cpu = ChooseMixedCpuMib(cfg);

  if (cfg.run_mem_cpu && !cancel.load(std::memory_order_relaxed)) {
    j.Set("cpu", RunCpuOnlyProbe(state_label, cfg, input, cancel, report));
    report.FlushJson();
  }
  if (cfg.run_mem_d3d11 && !cancel.load(std::memory_order_relaxed)) {
    j.Set("d3d11", RunGpuOrMixedProbe<D3D11MemProbe>("d3d11", 0, state_label, cfg, input, cancel, report));
    report.FlushJson();
  }
  if (cfg.run_mem_d3d12 && !cancel.load(std::memory_order_relaxed)) {
    j.Set("d3d12", RunGpuOrMixedProbe<D3D12MemProbe>("d3d12", 0, state_label, cfg, input, cancel, report));
    report.FlushJson();
  }
  if (cfg.run_mem_mixed_d3d11 && !cancel.load(std::memory_order_relaxed)) {
    j.Set("mixed_d3d11", RunGpuOrMixedProbe<D3D11MemProbe>("mixed_d3d11", mixed_cpu, state_label, cfg, input, cancel, report));
    report.FlushJson();
  }
  if (cfg.run_mem_mixed_d3d12 && !cancel.load(std::memory_order_relaxed)) {
    j.Set("mixed_d3d12", RunGpuOrMixedProbe<D3D12MemProbe>("mixed_d3d12", mixed_cpu, state_label, cfg, input, cancel, report));
    report.FlushJson();
  }

  j.Set("ms_since_last_input_end", JsonValue::Double(input.MsSinceLastInput()));
  return j;
}
