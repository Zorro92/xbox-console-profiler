#include "pch.h"
#include "BenchGpu.h"
#include "SystemInfo.h"

#include "shaders/GpuBenchAluCS.h"
#include "shaders/GpuBenchCopyCS.h"

namespace {

using Microsoft::WRL::ComPtr;

D3D12_RESOURCE_DESC MakeBufferDesc(UINT64 size, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  d.Alignment = 0;
  d.Width = size;
  d.Height = 1;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.Format = DXGI_FORMAT_UNKNOWN;
  d.SampleDesc.Count = 1;
  d.SampleDesc.Quality = 0;
  d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  d.Flags = flags;
  return d;
}

D3D12_HEAP_PROPERTIES MakeHeapProps(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES p{};
  p.Type = type;
  p.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
  p.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
  p.CreationNodeMask = 1;
  p.VisibleNodeMask = 1;
  return p;
}

D3D12_RESOURCE_BARRIER MakeTransition(ID3D12Resource* res, D3D12_RESOURCE_STATES before,
                                      D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = res;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = before;
  b.Transition.StateAfter = after;
  return b;
}

D3D12_RESOURCE_BARRIER MakeUavBarrier(ID3D12Resource* res) {
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  b.UAV.pResource = res;
  return b;
}

bool WaitFence(ID3D12CommandQueue* queue, ID3D12Fence* fence, HANDLE evt, uint64_t& val,
               DWORD timeout_ms = 15000) {
  ++val;
  if (FAILED(queue->Signal(fence, val))) return false;
  if (fence->GetCompletedValue() < val) {
    if (FAILED(fence->SetEventOnCompletion(val, evt))) return false;
    if (WaitForSingleObjectEx(evt, timeout_ms, FALSE) != WAIT_OBJECT_0) return false;
  }
  return true;
}

struct Stats {
  double min_v = 0.0;
  double p50_v = 0.0;
  double mean_v = 0.0;
  double p95_v = 0.0;
  double max_v = 0.0;
};

Stats ComputeStats(std::vector<double> vals) {
  Stats s;
  if (vals.empty()) return s;
  std::sort(vals.begin(), vals.end());
  s.min_v = vals.front();
  s.max_v = vals.back();
  s.p50_v = vals[vals.size() / 2];
  const double last_idx = static_cast<double>(vals.size() - 1);
  size_t i95 = static_cast<size_t>(std::min<double>(last_idx, std::floor(0.95 * last_idx)));
  s.p95_v = vals[i95];
  double sum = 0.0;
  for (double v : vals) sum += v;
  s.mean_v = sum / static_cast<double>(vals.size());
  return s;
}

JsonValue StatsToJson(const Stats& s) {
  JsonValue j = JsonValue::Object();
  j.Set("min_ms", JsonValue::Double(s.min_v));
  j.Set("p50_ms", JsonValue::Double(s.p50_v));
  j.Set("mean_ms", JsonValue::Double(s.mean_v));
  j.Set("p95_ms", JsonValue::Double(s.p95_v));
  j.Set("max_ms", JsonValue::Double(s.max_v));
  return j;
}

} // namespace

JsonValue RunGpuBenchmark(const std::string& state_label, const std::string& wake_mode,
                          bool use_warp, InputState& input, const std::atomic<bool>& cancel,
                          ProfilerReport& report) {
  JsonValue j = JsonValue::Object();
  j.Set("state", JsonValue::String(state_label));
  j.Set("ms_since_last_input_start", JsonValue::Double(input.MsSinceLastInput()));

  report.Log("--- GPU BANDWIDTH & TIME-SHARE BENCHMARK [" + state_label + "] (ms_since_input=" +
             std::to_string(static_cast<int>(input.MsSinceLastInput())) + ") ---");

  ComPtr<IDXGIFactory4> factory;
  ComPtr<IDXGIAdapter1> adapter;
  std::string err;
  if (!CreateAdapter(use_warp, factory, adapter, err)) {
    j.Set("error", JsonValue::String(err));
    report.Log("GPU ERROR: " + err);
    return j;
  }

  ComPtr<ID3D12Device> dev;
  HRESULT hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev));
  if (FAILED(hr)) {
    j.Set("error", JsonValue::String("D3D12CreateDevice failed: " + FormatHr(hr)));
    report.Log("GPU ERROR: D3D12CreateDevice failed " + FormatHr(hr));
    return j;
  }

  D3D12_FEATURE_DATA_ARCHITECTURE1 arch{};
  dev->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE1, &arch, sizeof(arch));
  const bool is_cc_uma = (arch.UMA != FALSE) && (arch.CacheCoherentUMA != FALSE);

  D3D12_COMMAND_QUEUE_DESC qdesc{};
  qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  ComPtr<ID3D12CommandQueue> queue;
  hr = dev->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&queue));
  if (FAILED(hr)) {
    j.Set("error", JsonValue::String("CreateCommandQueue failed: " + FormatHr(hr)));
    return j;
  }

  UINT64 ts_freq = 0;
  hr = queue->GetTimestampFrequency(&ts_freq);
  if (FAILED(hr) || ts_freq == 0) {
    j.Set("error", JsonValue::String("GetTimestampFrequency failed: " + FormatHr(hr)));
    return j;
  }
  j.Set("timestamp_frequency_hz", JsonValue::UInt(ts_freq));
  auto ticks_to_ms = [&](uint64_t ticks) -> double {
    return static_cast<double>(ticks) * 1000.0 / static_cast<double>(ts_freq);
  };

  ComPtr<ID3D12Fence> fence;
  hr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
  HANDLE fence_evt = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
  uint64_t fence_val = 0;
  if (FAILED(hr) || !fence_evt) {
    if (fence_evt) CloseHandle(fence_evt);
    j.Set("error", JsonValue::String("CreateFence/Event failed"));
    return j;
  }

  constexpr UINT kMaxQueries = 128;
  D3D12_QUERY_HEAP_DESC qhdesc{};
  qhdesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
  qhdesc.Count = kMaxQueries;
  ComPtr<ID3D12QueryHeap> query_heap;
  hr = dev->CreateQueryHeap(&qhdesc, IID_PPV_ARGS(&query_heap));

  ComPtr<ID3D12Resource> query_rb;
  auto rb_heap = MakeHeapProps(D3D12_HEAP_TYPE_READBACK);
  auto rb_desc = MakeBufferDesc(kMaxQueries * sizeof(uint64_t));
  if (SUCCEEDED(hr)) {
    hr = dev->CreateCommittedResource(&rb_heap, D3D12_HEAP_FLAG_NONE, &rb_desc,
                                      D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&query_rb));
  }

  ComPtr<ID3D12CommandAllocator> alloc0, alloc1;
  ComPtr<ID3D12GraphicsCommandList> list0, list1;
  if (SUCCEEDED(hr)) hr = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc0));
  if (SUCCEEDED(hr)) hr = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc1));
  if (SUCCEEDED(hr)) hr = dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc0.Get(), nullptr, IID_PPV_ARGS(&list0));
  if (SUCCEEDED(hr)) hr = dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc1.Get(), nullptr, IID_PPV_ARGS(&list1));
  if (SUCCEEDED(hr)) {
    list0->Close();
    list1->Close();
  } else {
    CloseHandle(fence_evt);
    j.Set("error", JsonValue::String("D3D12 setup failed: " + FormatHr(hr)));
    return j;
  }

  // Smaller 16 MiB buffer on WARP for fast CI smoke test; full 64 MiB on hardware.
  const UINT64 kBufBytes = use_warp ? (16ULL * 1024ULL * 1024ULL) : (64ULL * 1024ULL * 1024ULL);
  const double kBufGb = static_cast<double>(kBufBytes) / 1e9;
  j.Set("copy_buffer_mib", JsonValue::UInt(kBufBytes / (1024 * 1024)));

  auto def_heap = MakeHeapProps(D3D12_HEAP_TYPE_DEFAULT);
  auto up_heap = MakeHeapProps(D3D12_HEAP_TYPE_UPLOAD);
  auto def_uav_desc = MakeBufferDesc(kBufBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto plain_desc = MakeBufferDesc(kBufBytes);

  ComPtr<ID3D12Resource> def_a, def_b, upload_buf;
  hr = dev->CreateCommittedResource(&def_heap, D3D12_HEAP_FLAG_NONE, &def_uav_desc,
                                    D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&def_a));
  if (SUCCEEDED(hr)) {
    hr = dev->CreateCommittedResource(&def_heap, D3D12_HEAP_FLAG_NONE, &def_uav_desc,
                                      D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&def_b));
  }
  if (SUCCEEDED(hr)) {
    hr = dev->CreateCommittedResource(&up_heap, D3D12_HEAP_FLAG_NONE, &plain_desc,
                                      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload_buf));
  }
  if (FAILED(hr)) {
    CloseHandle(fence_evt);
    j.Set("error", JsonValue::String("Failed to allocate GPU bench buffers: " + FormatHr(hr)));
    return j;
  }

  // Helper to time N CopyResource operations from src -> dst.
  auto time_copy = [&](ID3D12Resource* src, ID3D12Resource* dst, int iters,
                       D3D12_RESOURCE_STATES src_state, D3D12_RESOURCE_STATES dst_state,
                       bool restore_states) -> JsonValue {
    JsonValue jc = JsonValue::Object();
    if (state_label == "active") input.KeepAliveIfNeeded(wake_mode, cancel);

    alloc0->Reset();
    list0->Reset(alloc0.Get(), nullptr);

    D3D12_RESOURCE_BARRIER pre[2];
    int npre = 0;
    if (src_state != D3D12_RESOURCE_STATE_COPY_SOURCE && src_state != D3D12_RESOURCE_STATE_GENERIC_READ) {
      pre[npre++] = MakeTransition(src, src_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    if (dst_state != D3D12_RESOURCE_STATE_COPY_DEST) {
      pre[npre++] = MakeTransition(dst, dst_state, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    if (npre > 0) list0->ResourceBarrier(static_cast<UINT>(npre), pre);

    // Warmup 2 copies
    for (int w = 0; w < 2; ++w) list0->CopyResource(dst, src);

    list0->EndQuery(query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
    for (int i = 0; i < iters; ++i) {
      list0->CopyResource(dst, src);
      list0->EndQuery(query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(i + 1));
    }
    list0->ResolveQueryData(query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0,
                            static_cast<UINT>(iters + 1), query_rb.Get(), 0);

    if (restore_states && npre > 0) {
      for (int i = 0; i < npre; ++i) std::swap(pre[i].Transition.StateBefore, pre[i].Transition.StateAfter);
      list0->ResourceBarrier(static_cast<UINT>(npre), pre);
    }
    list0->Close();

    ID3D12CommandList* cls[] = {list0.Get()};
    int64_t w0 = InputState::NowQpc();
    queue->ExecuteCommandLists(1, cls);
    bool ok = WaitFence(queue.Get(), fence.Get(), fence_evt, fence_val);
    double wall_ms = InputState::QpcToMs(InputState::NowQpc() - w0);
    if (!ok) {
      jc.Set("error", JsonValue::String("GPU fence timeout"));
      return jc;
    }

    uint64_t* ts = nullptr;
    D3D12_RANGE range{0, static_cast<SIZE_T>((iters + 1) * sizeof(uint64_t))};
    if (SUCCEEDED(query_rb->Map(0, &range, reinterpret_cast<void**>(&ts)))) {
      std::vector<double> dms;
      for (int i = 0; i < iters; ++i) {
        if (ts[i + 1] >= ts[i]) dms.push_back(ticks_to_ms(ts[i + 1] - ts[i]));
      }
      D3D12_RANGE empty{0, 0};
      query_rb->Unmap(0, &empty);
      Stats st = ComputeStats(dms);
      double payload_gbps = st.mean_v > 0.0 ? (kBufGb / (st.mean_v / 1000.0)) : 0.0;
      double rw_gbps = payload_gbps * 2.0;
      double ms_25 = payload_gbps > 0.0 ? (24.9 / payload_gbps) : 0.0;
      jc.Set("iters", JsonValue::Int(iters));
      jc.Set("gpu_stats", StatsToJson(st));
      jc.Set("wall_ms_including_warmup", JsonValue::Double(wall_ms));
      jc.Set("payload_gbps", JsonValue::Double(payload_gbps));
      jc.Set("rw_gbps", JsonValue::Double(rw_gbps));
      jc.Set("ms_per_25mb", JsonValue::Double(ms_25));
    }
    return jc;
  };

  // 1. Upload buffer CPU fill/read + GPU UPLOAD -> DEFAULT copy
  {
    void* mapped = nullptr;
    D3D12_RANGE read_all{0, static_cast<SIZE_T>(kBufBytes)};
    double cpu_fill_gbps = 0.0, cpu_read_gbps = 0.0;
    if (SUCCEEDED(upload_buf->Map(0, &read_all, &mapped))) {
      int64_t t0 = InputState::NowQpc();
      std::memset(mapped, 0x5A, static_cast<size_t>(kBufBytes));
      double fill_s = InputState::QpcToMs(InputState::NowQpc() - t0) / 1000.0;
      cpu_fill_gbps = fill_s > 0.0 ? (kBufGb / fill_s) : 0.0;

      int64_t t1 = InputState::NowQpc();
      const uint64_t* p64 = static_cast<const uint64_t*>(mapped);
      uint64_t acc = 0;
      size_t n64 = static_cast<size_t>(kBufBytes / 8);
      for (size_t i = 0; i < n64; ++i) acc ^= p64[i];
      double read_s = InputState::QpcToMs(InputState::NowQpc() - t1) / 1000.0;
      cpu_read_gbps = read_s > 0.0 ? (kBufGb / read_s) : 0.0;
      (void)acc;
      // Keep persistently mapped while GPU copies (valid in D3D12)
    }
    int iters = use_warp ? 2 : 6;
    JsonValue jup = time_copy(upload_buf.Get(), def_a.Get(), iters,
                              D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COMMON, true);
    jup.Set("cpu_1t_fill_gbps", JsonValue::Double(cpu_fill_gbps));
    jup.Set("cpu_1t_read_gbps", JsonValue::Double(cpu_read_gbps));
    if (mapped) {
      D3D12_RANGE empty{0, 0};
      upload_buf->Unmap(0, &empty);
    }
    report.Log("GPU UPLOAD->DEFAULT (" + std::to_string(kBufBytes >> 20) + " MiB): " + jup.Dump(0));
    j.Set("upload_to_default_copy", std::move(jup));
  }

  // 2. GPU DEFAULT -> DEFAULT copy
  {
    int iters = use_warp ? 2 : 8;
    JsonValue jdd = time_copy(def_a.Get(), def_b.Get(), iters,
                              D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON, true);
    report.Log("GPU DEFAULT->DEFAULT (" + std::to_string(kBufBytes >> 20) + " MiB): " + jdd.Dump(0));
    j.Set("default_to_default_copy", std::move(jdd));
  }

  // 3. Cache-coherent UMA CUSTOM write-back L0 heap (zero-copy candidate on Xbox)
  if (is_cc_uma && !cancel.load(std::memory_order_relaxed)) {
    D3D12_HEAP_PROPERTIES custom_hp{};
    custom_hp.Type = D3D12_HEAP_TYPE_CUSTOM;
    custom_hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
    custom_hp.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
    custom_hp.CreationNodeMask = 1;
    custom_hp.VisibleNodeMask = 1;
    ComPtr<ID3D12Resource> custom_buf;
    HRESULT chr = dev->CreateCommittedResource(&custom_hp, D3D12_HEAP_FLAG_NONE, &plain_desc,
                                               D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr,
                                               IID_PPV_ARGS(&custom_buf));
    JsonValue jcust = JsonValue::Object();
    jcust.Set("create_hr", JsonValue::String(FormatHr(chr)));
    if (SUCCEEDED(chr)) {
      void* mapped = nullptr;
      D3D12_RANGE read_all{0, static_cast<SIZE_T>(kBufBytes)};
      HRESULT mhr = custom_buf->Map(0, &read_all, &mapped);
      jcust.Set("map_hr", JsonValue::String(FormatHr(mhr)));
      if (SUCCEEDED(mhr) && mapped) {
        int64_t t0 = InputState::NowQpc();
        std::memset(mapped, 0x3C, static_cast<size_t>(kBufBytes));
        double fill_s = InputState::QpcToMs(InputState::NowQpc() - t0) / 1000.0;
        jcust.Set("cpu_1t_fill_gbps", JsonValue::Double(fill_s > 0.0 ? (kBufGb / fill_s) : 0.0));

        int64_t t1 = InputState::NowQpc();
        const uint64_t* p64 = static_cast<const uint64_t*>(mapped);
        uint64_t acc = 0;
        size_t n64 = static_cast<size_t>(kBufBytes / 8);
        for (size_t i = 0; i < n64; ++i) acc ^= p64[i];
        double read_s = InputState::QpcToMs(InputState::NowQpc() - t1) / 1000.0;
        jcust.Set("cpu_1t_read_gbps", JsonValue::Double(read_s > 0.0 ? (kBufGb / read_s) : 0.0));
        (void)acc;

        JsonValue gcopy = time_copy(custom_buf.Get(), def_b.Get(), use_warp ? 2 : 6,
                                    D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON, true);
        jcust.Set("gpu_copy", std::move(gcopy));
        D3D12_RANGE empty{0, 0};
        custom_buf->Unmap(0, &empty);
      }
    }
    report.Log("GPU UMA CUSTOM_WB_L0: " + jcust.Dump(0));
    j.Set("uma_custom_wb_l0", std::move(jcust));
  }

  // 4. Compute pipelines: root signature = [0: 4 root 32-bit constants (b0), 1: root SRV (t0), 2: root UAV (u0)]
  D3D12_ROOT_PARAMETER params[3]{};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[0].Constants.ShaderRegister = 0;
  params[0].Constants.Num32BitValues = 4;
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  params[1].Descriptor.ShaderRegister = 0;
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

  params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  params[2].Descriptor.ShaderRegister = 0;
  params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

  D3D12_ROOT_SIGNATURE_DESC rs_desc{};
  rs_desc.NumParameters = 3;
  rs_desc.pParameters = params;

  ComPtr<ID3DBlob> rs_blob, rs_err;
  ComPtr<ID3D12RootSignature> root_sig;
  hr = D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &rs_err);
  if (SUCCEEDED(hr)) {
    hr = dev->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
                                  IID_PPV_ARGS(&root_sig));
  }

  ComPtr<ID3D12PipelineState> pso_copy, pso_alu;
  if (SUCCEEDED(hr)) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC pdesc{};
    pdesc.pRootSignature = root_sig.Get();
    pdesc.CS = {g_GpuBenchCopyCS, sizeof(g_GpuBenchCopyCS)};
    hr = dev->CreateComputePipelineState(&pdesc, IID_PPV_ARGS(&pso_copy));
  }
  if (SUCCEEDED(hr)) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC pdesc{};
    pdesc.pRootSignature = root_sig.Get();
    pdesc.CS = {g_GpuBenchAluCS, sizeof(g_GpuBenchAluCS)};
    hr = dev->CreateComputePipelineState(&pdesc, IID_PPV_ARGS(&pso_alu));
  }

  if (FAILED(hr)) {
    j.Set("compute_error", JsonValue::String("Compute PSO creation failed: " + FormatHr(hr)));
    report.Log("GPU COMPUTE ERROR: " + FormatHr(hr));
    CloseHandle(fence_evt);
    return j;
  }

  // 4a. Compute copy bandwidth (cs_copy: 64 MiB -> 64 MiB)
  if (!cancel.load(std::memory_order_relaxed)) {
    if (state_label == "active") input.KeepAliveIfNeeded(wake_mode, cancel);
    const UINT count_vec4 = static_cast<UINT>(kBufBytes / 16ULL);
    const UINT groups = (count_vec4 + 255u) / 256u;
    const int iters = use_warp ? 2 : 6;
    struct RootConsts {
      uint32_t count_vec4;
      uint32_t iterations;
      float k_mul;
      float k_add;
    } rc{count_vec4, 1u, -0.9991f, 0.0005f};

    alloc0->Reset();
    list0->Reset(alloc0.Get(), pso_copy.Get());
    list0->SetComputeRootSignature(root_sig.Get());
    list0->SetComputeRoot32BitConstants(0, 4, &rc, 0);
    list0->SetComputeRootShaderResourceView(1, def_a->GetGPUVirtualAddress());
    list0->SetComputeRootUnorderedAccessView(2, def_b->GetGPUVirtualAddress());

    D3D12_RESOURCE_BARRIER pre[2] = {
        MakeTransition(def_a.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        MakeTransition(def_b.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
    list0->ResourceBarrier(2, pre);

    // Warmup
    list0->Dispatch(groups, 1, 1);
    D3D12_RESOURCE_BARRIER uavb = MakeUavBarrier(def_b.Get());
    list0->ResourceBarrier(1, &uavb);

    list0->EndQuery(query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
    for (int i = 0; i < iters; ++i) {
      list0->Dispatch(groups, 1, 1);
      list0->ResourceBarrier(1, &uavb);
      list0->EndQuery(query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(i + 1));
    }
    list0->ResolveQueryData(query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0,
                            static_cast<UINT>(iters + 1), query_rb.Get(), 0);
    std::swap(pre[0].Transition.StateBefore, pre[0].Transition.StateAfter);
    std::swap(pre[1].Transition.StateBefore, pre[1].Transition.StateAfter);
    list0->ResourceBarrier(2, pre);
    list0->Close();

    ID3D12CommandList* cls[] = {list0.Get()};
    int64_t w0 = InputState::NowQpc();
    queue->ExecuteCommandLists(1, cls);
    bool ok = WaitFence(queue.Get(), fence.Get(), fence_evt, fence_val);
    double wall_ms = InputState::QpcToMs(InputState::NowQpc() - w0);

    JsonValue jcs = JsonValue::Object();
    if (ok) {
      uint64_t* ts = nullptr;
      D3D12_RANGE range{0, static_cast<SIZE_T>((iters + 1) * sizeof(uint64_t))};
      if (SUCCEEDED(query_rb->Map(0, &range, reinterpret_cast<void**>(&ts)))) {
        std::vector<double> dms;
        for (int i = 0; i < iters; ++i) {
          if (ts[i + 1] >= ts[i]) dms.push_back(ticks_to_ms(ts[i + 1] - ts[i]));
        }
        D3D12_RANGE empty{0, 0};
        query_rb->Unmap(0, &empty);
        Stats st = ComputeStats(dms);
        double payload_gbps = st.mean_v > 0.0 ? (kBufGb / (st.mean_v / 1000.0)) : 0.0;
        jcs.Set("iters", JsonValue::Int(iters));
        jcs.Set("gpu_stats", StatsToJson(st));
        jcs.Set("wall_ms_including_warmup", JsonValue::Double(wall_ms));
        jcs.Set("payload_gbps", JsonValue::Double(payload_gbps));
        jcs.Set("rw_gbps", JsonValue::Double(payload_gbps * 2.0));
      }
    } else {
      jcs.Set("error", JsonValue::String("Compute copy fence timeout"));
    }
    report.Log("GPU COMPUTE COPY (" + std::to_string(kBufBytes >> 20) + " MiB): " + jcs.Dump(0));
    j.Set("compute_copy_bandwidth", std::move(jcs));
  }

  // 4b. GPU Time-Share & ALU throughput probe (fixed FP32 FMA workload: burst + sustained pipelined)
  if (!cancel.load(std::memory_order_relaxed)) {
    // Fixed workload per dispatch:
    // Hardware: 1024 groups * 256 threads * 2048 iters * 64 FLOPs = 34.3597 GFLOPs/dispatch
    //   (~2.8 ms on full 12 TFLOP/s Series X GPU, ~6.3 ms at 45% App GPU share).
    // WARP:     32 groups * 256 threads * 64 iters * 64 FLOPs = 0.03355 GFLOPs/dispatch.
    const UINT alu_groups = use_warp ? 32u : 1024u;
    const UINT alu_iters = use_warp ? 64u : 2048u;
    const double gflops_per_dispatch =
        static_cast<double>(alu_groups) * 256.0 * static_cast<double>(alu_iters) * 64.0 / 1e9;

    struct RootConsts {
      uint32_t count_vec4;
      uint32_t iterations;
      float k_mul;
      float k_add;
    } rc{256u, alu_iters, -0.9991f, 0.0005f};

    JsonValue jts = JsonValue::Object();
    jts.Set("groups", JsonValue::UInt(alu_groups));
    jts.Set("threads_per_group", JsonValue::UInt(256));
    jts.Set("alu_loop_iters", JsonValue::UInt(alu_iters));
    jts.Set("gflops_per_dispatch", JsonValue::Double(gflops_per_dispatch));

    // Burst: N back-to-back dispatches in one command list with timestamp before & after each.
    {
      if (state_label == "active") input.KeepAliveIfNeeded(wake_mode, cancel);
      const int n_disp = use_warp ? 4 : 20;
      alloc0->Reset();
      list0->Reset(alloc0.Get(), pso_alu.Get());
      list0->SetComputeRootSignature(root_sig.Get());
      list0->SetComputeRoot32BitConstants(0, 4, &rc, 0);
      list0->SetComputeRootShaderResourceView(1, def_a->GetGPUVirtualAddress());
      list0->SetComputeRootUnorderedAccessView(2, def_b->GetGPUVirtualAddress());
      D3D12_RESOURCE_BARRIER to_uav = MakeTransition(def_b.Get(), D3D12_RESOURCE_STATE_COMMON,
                                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      list0->ResourceBarrier(1, &to_uav);
      D3D12_RESOURCE_BARRIER uavb = MakeUavBarrier(def_b.Get());

      // 1 warmup dispatch
      list0->Dispatch(alu_groups, 1, 1);
      list0->ResourceBarrier(1, &uavb);

      for (int i = 0; i < n_disp; ++i) {
        list0->EndQuery(query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(2 * i));
        list0->Dispatch(alu_groups, 1, 1);
        list0->ResourceBarrier(1, &uavb);
        list0->EndQuery(query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(2 * i + 1));
      }
      list0->ResolveQueryData(query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0,
                              static_cast<UINT>(2 * n_disp), query_rb.Get(), 0);
      std::swap(to_uav.Transition.StateBefore, to_uav.Transition.StateAfter);
      list0->ResourceBarrier(1, &to_uav);
      list0->Close();

      ID3D12CommandList* cls[] = {list0.Get()};
      int64_t w0 = InputState::NowQpc();
      queue->ExecuteCommandLists(1, cls);
      bool ok = WaitFence(queue.Get(), fence.Get(), fence_evt, fence_val);
      double wall_ms = InputState::QpcToMs(InputState::NowQpc() - w0);

      JsonValue jburst = JsonValue::Object();
      if (ok) {
        uint64_t* ts = nullptr;
        D3D12_RANGE range{0, static_cast<SIZE_T>(2 * n_disp * sizeof(uint64_t))};
        if (SUCCEEDED(query_rb->Map(0, &range, reinterpret_cast<void**>(&ts)))) {
          std::vector<double> dms;
          double sum_gpu = 0.0;
          double sum_gap = 0.0;
          for (int i = 0; i < n_disp; ++i) {
            double d = ts[2 * i + 1] >= ts[2 * i] ? ticks_to_ms(ts[2 * i + 1] - ts[2 * i]) : 0.0;
            dms.push_back(d);
            sum_gpu += d;
            if (i > 0 && ts[2 * i] >= ts[2 * i - 1]) {
              sum_gap += ticks_to_ms(ts[2 * i] - ts[2 * i - 1]);
            }
          }
          double span_ms = ts[2 * n_disp - 1] >= ts[0] ? ticks_to_ms(ts[2 * n_disp - 1] - ts[0]) : 0.0;
          D3D12_RANGE empty{0, 0};
          query_rb->Unmap(0, &empty);

          Stats st = ComputeStats(dms);
          double total_gflops = gflops_per_dispatch * static_cast<double>(n_disp);
          double span_tflops = span_ms > 0.0 ? (total_gflops / (span_ms / 1000.0) / 1000.0) : 0.0;
          double wall_tflops = wall_ms > 0.0 ? ((gflops_per_dispatch * (n_disp + 1)) / (wall_ms / 1000.0) / 1000.0) : 0.0;
          double min_tflops = st.min_v > 0.0 ? (gflops_per_dispatch / (st.min_v / 1000.0) / 1000.0) : 0.0;

          jburst.Set("dispatches", JsonValue::Int(n_disp));
          jburst.Set("per_dispatch_gpu", StatsToJson(st));
          jburst.Set("min_over_mean_ratio", JsonValue::Double(st.mean_v > 0.0 ? (st.min_v / st.mean_v) : 0.0));
          jburst.Set("sum_dispatch_gpu_ms", JsonValue::Double(sum_gpu));
          jburst.Set("inter_dispatch_gap_ms", JsonValue::Double(sum_gap));
          jburst.Set("gpu_span_ms", JsonValue::Double(span_ms));
          jburst.Set("wall_ms_including_1_warmup", JsonValue::Double(wall_ms));
          jburst.Set("gpu_span_over_wall_est",
                     JsonValue::Double(wall_ms > 0.0 ? (span_ms * (n_disp + 1.0) / n_disp / wall_ms) : 0.0));
          jburst.Set("burst_gpu_span_tflops", JsonValue::Double(span_tflops));
          jburst.Set("burst_wall_tflops", JsonValue::Double(wall_tflops));
          jburst.Set("best_single_dispatch_tflops", JsonValue::Double(min_tflops));

          char buf[260];
          std::snprintf(buf, sizeof(buf),
                        "GPU ALU burst (%d x %.2f GF): disp min=%.2f p50=%.2f mean=%.2f max=%.2f ms | "
                        "span=%.1f ms wall=%.1f ms | span=%.2f TFLOP/s best_disp=%.2f TFLOP/s min/mean=%.3f",
                        n_disp, gflops_per_dispatch, st.min_v, st.p50_v, st.mean_v, st.max_v,
                        span_ms, wall_ms, span_tflops, min_tflops,
                        st.mean_v > 0.0 ? st.min_v / st.mean_v : 0.0);
          report.Log(buf);
        }
      } else {
        jburst.Set("error", JsonValue::String("Burst fence timeout"));
      }
      jts.Set("burst", std::move(jburst));
    }

    // Sustained pipelined submissions (2-deep queue saturation) to measure long-window GPU share vs wall clock.
    {
      if (state_label == "active") input.KeepAliveIfNeeded(wake_mode, cancel);
      const int n_subs = use_warp ? 4 : 32;
      const int disps_per_sub = use_warp ? 1 : 2;
      ComPtr<ID3D12CommandAllocator> allocs[2] = {alloc0, alloc1};
      ComPtr<ID3D12GraphicsCommandList> lists[2] = {list0, list1};
      uint64_t slot_fence[2] = {0, 0};

      // Transition def_b to UAV once for the sustained window.
      alloc0->Reset();
      list0->Reset(alloc0.Get(), nullptr);
      D3D12_RESOURCE_BARRIER to_uav = MakeTransition(def_b.Get(), D3D12_RESOURCE_STATE_COMMON,
                                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      list0->ResourceBarrier(1, &to_uav);
      list0->Close();
      ID3D12CommandList* init_cls[] = {list0.Get()};
      queue->ExecuteCommandLists(1, init_cls);
      WaitFence(queue.Get(), fence.Get(), fence_evt, fence_val);

      D3D12_RESOURCE_BARRIER uavb = MakeUavBarrier(def_b.Get());
      std::vector<double> sub_gpu_ms;
      bool ok = true;

      int64_t w0 = InputState::NowQpc();
      for (int s = 0; s < n_subs; ++s) {
        int slot = s & 1;
        if (slot_fence[slot] > 0) {
          if (fence->GetCompletedValue() < slot_fence[slot]) {
            fence->SetEventOnCompletion(slot_fence[slot], fence_evt);
            if (WaitForSingleObjectEx(fence_evt, 15000, FALSE) != WAIT_OBJECT_0) {
              ok = false;
              break;
            }
          }
          // Read timestamp of the completed previous submission on this slot.
          uint64_t* ts = nullptr;
          UINT base_q = static_cast<UINT>(slot * 2);
          D3D12_RANGE rng{base_q * sizeof(uint64_t), (base_q + 2) * sizeof(uint64_t)};
          if (SUCCEEDED(query_rb->Map(0, &rng, reinterpret_cast<void**>(&ts)))) {
            if (ts[base_q + 1] >= ts[base_q]) {
              sub_gpu_ms.push_back(ticks_to_ms(ts[base_q + 1] - ts[base_q]));
            }
            D3D12_RANGE empty{0, 0};
            query_rb->Unmap(0, &empty);
          }
        }

        UINT base_q = static_cast<UINT>(slot * 2);
        allocs[slot]->Reset();
        lists[slot]->Reset(allocs[slot].Get(), pso_alu.Get());
        lists[slot]->SetComputeRootSignature(root_sig.Get());
        lists[slot]->SetComputeRoot32BitConstants(0, 4, &rc, 0);
        lists[slot]->SetComputeRootShaderResourceView(1, def_a->GetGPUVirtualAddress());
        lists[slot]->SetComputeRootUnorderedAccessView(2, def_b->GetGPUVirtualAddress());
        lists[slot]->EndQuery(query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, base_q);
        for (int k = 0; k < disps_per_sub; ++k) {
          lists[slot]->Dispatch(alu_groups, 1, 1);
          lists[slot]->ResourceBarrier(1, &uavb);
        }
        lists[slot]->EndQuery(query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, base_q + 1);
        lists[slot]->ResolveQueryData(query_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, base_q, 2,
                                      query_rb.Get(), base_q * sizeof(uint64_t));
        lists[slot]->Close();

        ID3D12CommandList* cls[] = {lists[slot].Get()};
        queue->ExecuteCommandLists(1, cls);
        ++fence_val;
        queue->Signal(fence.Get(), fence_val);
        slot_fence[slot] = fence_val;
      }

      // Drain remaining in-flight slots
      for (int slot = 0; slot < 2 && ok; ++slot) {
        if (slot_fence[slot] > 0) {
          if (fence->GetCompletedValue() < slot_fence[slot]) {
            fence->SetEventOnCompletion(slot_fence[slot], fence_evt);
            if (WaitForSingleObjectEx(fence_evt, 15000, FALSE) != WAIT_OBJECT_0) {
              ok = false;
              break;
            }
          }
          uint64_t* ts = nullptr;
          UINT base_q = static_cast<UINT>(slot * 2);
          D3D12_RANGE rng{base_q * sizeof(uint64_t), (base_q + 2) * sizeof(uint64_t)};
          if (SUCCEEDED(query_rb->Map(0, &rng, reinterpret_cast<void**>(&ts)))) {
            if (ts[base_q + 1] >= ts[base_q]) {
              sub_gpu_ms.push_back(ticks_to_ms(ts[base_q + 1] - ts[base_q]));
            }
            D3D12_RANGE empty{0, 0};
            query_rb->Unmap(0, &empty);
          }
        }
      }
      double wall_ms = InputState::QpcToMs(InputState::NowQpc() - w0);

      JsonValue jsus = JsonValue::Object();
      if (ok) {
        Stats st = ComputeStats(sub_gpu_ms);
        double total_gflops = gflops_per_dispatch * static_cast<double>(n_subs * disps_per_sub);
        double sum_gpu = 0.0;
        for (double d : sub_gpu_ms) sum_gpu += d;
        double wall_tflops = wall_ms > 0.0 ? (total_gflops / (wall_ms / 1000.0) / 1000.0) : 0.0;
        double gpu_tflops = sum_gpu > 0.0 ? (total_gflops / (sum_gpu / 1000.0) / 1000.0) : 0.0;

        jsus.Set("submissions", JsonValue::Int(n_subs));
        jsus.Set("dispatches_per_sub", JsonValue::Int(disps_per_sub));
        jsus.Set("total_wall_ms", JsonValue::Double(wall_ms));
        jsus.Set("sum_sub_gpu_ms", JsonValue::Double(sum_gpu));
        jsus.Set("gpu_over_wall_ratio", JsonValue::Double(wall_ms > 0.0 ? (sum_gpu / wall_ms) : 0.0));
        jsus.Set("per_sub_gpu", StatsToJson(st));
        jsus.Set("sustained_wall_tflops", JsonValue::Double(wall_tflops));
        jsus.Set("sustained_gpu_tflops", JsonValue::Double(gpu_tflops));

        char buf[260];
        std::snprintf(buf, sizeof(buf),
                      "GPU ALU sustained (%d x %d disp): wall=%.1f ms sum_gpu=%.1f ms (gpu/wall=%.3f) | "
                      "sub min=%.2f p50=%.2f p95=%.2f max=%.2f ms | wall=%.2f TFLOP/s",
                      n_subs, disps_per_sub, wall_ms, sum_gpu,
                      wall_ms > 0.0 ? sum_gpu / wall_ms : 0.0,
                      st.min_v, st.p50_v, st.p95_v, st.max_v, wall_tflops);
        report.Log(buf);
      } else {
        jsus.Set("error", JsonValue::String("Sustained fence timeout"));
      }
      jts.Set("sustained", std::move(jsus));
    }

    j.Set("gpu_time_share", std::move(jts));
  }

  CloseHandle(fence_evt);
  j.Set("ms_since_last_input_end", JsonValue::Double(input.MsSinceLastInput()));
  return j;
}
