#include "pch.h"
#include "SystemInfo.h"

#include <intrin.h>

namespace {

std::atomic<uint64_t> g_ev_usage_increased{0};
std::atomic<uint64_t> g_ev_usage_decreased{0};
std::atomic<uint64_t> g_ev_limit_changing{0};
std::atomic<bool> g_events_registered{false};

std::string WideToUtf8(const wchar_t* ws) {
  if (!ws || !*ws) return {};
  int needed = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
  if (needed <= 1) return {};
  std::string out(static_cast<size_t>(needed - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, ws, -1, out.data(), needed, nullptr, nullptr);
  return out;
}

#if defined(WINAPI_FAMILY) && (WINAPI_FAMILY == WINAPI_FAMILY_APP)
std::string PlatformStringToUtf8(Platform::String^ s) {
  if (s == nullptr) return {};
  return WideToUtf8(s->Data());
}

// Xbox-specific extended memory info (mirrors rav1g's LogAppMemoryPools).
struct XboxAppMemoryPools {
  uint64_t app_usage;
  uint64_t app_limit;
  uint64_t extended_usage;
  uint64_t extended_limit;
};

struct IAppResourcesStatics2 : public ::IInspectable {
  virtual HRESULT STDMETHODCALLTYPE GetExtendedMemoryInfo(XboxAppMemoryPools* pools) = 0;
};

const IID kAppResourcesStatics2Iid = {
    0xdd36a017, 0xb640, 0x45f7, {0xa0, 0x23, 0x16, 0x15, 0xcf, 0x09, 0x89, 0x23}};

HRESULT QueryXboxExtendedPools(XboxAppMemoryPools& out) {
  std::memset(&out, 0, sizeof(out));
  static const wchar_t kClass[] = L"Windows.Xbox.ApplicationModel.ApplicationResources";
  HSTRING_HEADER header{};
  HSTRING name = nullptr;
  HRESULT hr = ::WindowsCreateStringReference(
      kClass, static_cast<UINT32>((sizeof(kClass) / sizeof(kClass[0])) - 1), &header, &name);
  if (FAILED(hr)) return hr;
  IAppResourcesStatics2* statics = nullptr;
  hr = ::RoGetActivationFactory(name, kAppResourcesStatics2Iid, reinterpret_cast<void**>(&statics));
  if (SUCCEEDED(hr) && statics) {
    hr = statics->GetExtendedMemoryInfo(&out);
    statics->Release();
  }
  return hr;
}
#endif

std::string InferVariantTag(const std::string& identity) {
  std::string low = identity;
  for (char& c : low) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  if (low.find("hevc") != std::string::npos) return "hevc";
  if (low.find("game") != std::string::npos) return "game";
  if (low.find("expanded") != std::string::npos) return "expanded";
  if (low.find("plain") != std::string::npos) return "plain";
  return "app";
}

} // namespace

void RegisterMemoryManagerEvents() {
  if (g_events_registered.exchange(true)) return;
#if defined(WINAPI_FAMILY) && (WINAPI_FAMILY == WINAPI_FAMILY_APP)
  try {
    using namespace Windows::System;
    using namespace Windows::Foundation;
    MemoryManager::AppMemoryUsageIncreased +=
        ref new EventHandler<Platform::Object^>([](Platform::Object^, Platform::Object^) {
          g_ev_usage_increased.fetch_add(1, std::memory_order_relaxed);
        });
    MemoryManager::AppMemoryUsageDecreased +=
        ref new EventHandler<Platform::Object^>([](Platform::Object^, Platform::Object^) {
          g_ev_usage_decreased.fetch_add(1, std::memory_order_relaxed);
        });
    MemoryManager::AppMemoryUsageLimitChanging +=
        ref new EventHandler<AppMemoryUsageLimitChangingEventArgs^>(
            [](Platform::Object^, AppMemoryUsageLimitChangingEventArgs^) {
              g_ev_limit_changing.fetch_add(1, std::memory_order_relaxed);
            });
  } catch (...) {
  }
#endif
}

PackageMeta QueryPackageMeta() {
  PackageMeta meta;
#if defined(WINAPI_FAMILY) && (WINAPI_FAMILY == WINAPI_FAMILY_APP)
  try {
    auto pkg = Windows::ApplicationModel::Package::Current;
    auto id = pkg->Id;
    meta.identity_name = PlatformStringToUtf8(id->Name);
    meta.full_name = PlatformStringToUtf8(id->FullName);
    meta.family_name = PlatformStringToUtf8(id->FamilyName);
    meta.display_name = PlatformStringToUtf8(pkg->DisplayName);
    auto ver = id->Version;
    char vbuf[64];
    std::snprintf(vbuf, sizeof(vbuf), "%u.%u.%u.%u", ver.Major, ver.Minor, ver.Build, ver.Revision);
    meta.version = vbuf;
    meta.variant_tag = InferVariantTag(meta.identity_name);
  } catch (...) {
  }
  try {
    auto vi = Windows::System::Profile::AnalyticsInfo::VersionInfo;
    meta.device_family = PlatformStringToUtf8(vi->DeviceFamily);
    uint64_t v = std::strtoull(PlatformStringToUtf8(vi->DeviceFamilyVersion).c_str(), nullptr, 10);
    char vbuf[64];
    std::snprintf(vbuf, sizeof(vbuf), "%u.%u.%u.%u",
                  static_cast<unsigned>((v >> 48) & 0xFFFF),
                  static_cast<unsigned>((v >> 32) & 0xFFFF),
                  static_cast<unsigned>((v >> 16) & 0xFFFF),
                  static_cast<unsigned>(v & 0xFFFF));
    meta.device_family_version = vbuf;
  } catch (...) {
  }
  try {
    auto folder = Windows::Storage::ApplicationData::Current->LocalFolder;
    meta.local_state_dir = PlatformStringToUtf8(folder->Path);
  } catch (...) {
  }
#else
  char cwd[MAX_PATH] = {};
  GetCurrentDirectoryA(MAX_PATH, cwd);
  meta.local_state_dir = cwd;
#endif
  return meta;
}

MemorySnapshot CaptureMemorySnapshot() {
  MemorySnapshot m;
  m.ev_usage_increased = g_ev_usage_increased.load(std::memory_order_relaxed);
  m.ev_usage_decreased = g_ev_usage_decreased.load(std::memory_order_relaxed);
  m.ev_limit_changing = g_ev_limit_changing.load(std::memory_order_relaxed);

#if defined(WINAPI_FAMILY) && (WINAPI_FAMILY == WINAPI_FAMILY_APP)
  try {
    using namespace Windows::System;
    m.app_usage = MemoryManager::AppMemoryUsage;
    m.app_limit = MemoryManager::AppMemoryUsageLimit;
    auto lvl = MemoryManager::AppMemoryUsageLevel;
    switch (lvl) {
      case AppMemoryUsageLevel::Low: m.app_level = "Low"; break;
      case AppMemoryUsageLevel::Medium: m.app_level = "Medium"; break;
      case AppMemoryUsageLevel::High: m.app_level = "High"; break;
      case AppMemoryUsageLevel::OverLimit: m.app_level = "OverLimit"; break;
      default: m.app_level = "Unknown"; break;
    }
    auto report = MemoryManager::GetAppMemoryReport();
    if (report != nullptr) {
      m.private_commit_usage = report->PrivateCommitUsage;
      m.total_commit_usage = report->TotalCommitUsage;
      m.total_commit_limit = report->TotalCommitLimit;
      m.expected_total_commit_limit = report->ExpectedTotalCommitLimit;
    }
    auto proc = MemoryManager::GetProcessMemoryReport();
    if (proc != nullptr) {
      m.private_working_set_usage = proc->PrivateWorkingSetUsage;
    }
    m.memory_manager_ok = true;
  } catch (...) {
    m.memory_manager_ok = false;
  }

  XboxAppMemoryPools pools{};
  m.xbox_pools_hr = QueryXboxExtendedPools(pools);
  if (SUCCEEDED(m.xbox_pools_hr)) {
    m.xbox_pools_ok = true;
    m.xbox_app_usage = pools.app_usage;
    m.xbox_app_limit = pools.app_limit;
    m.xbox_ext_usage = pools.extended_usage;
    m.xbox_ext_limit = pools.extended_limit;
  }
#else
  MEMORYSTATUSEX st{};
  st.dwLength = sizeof(st);
  if (GlobalMemoryStatusEx(&st)) {
    m.memory_manager_ok = true;
    m.app_limit = st.ullTotalPhys;
    m.app_usage = st.ullTotalPhys - st.ullAvailPhys;
    m.total_commit_limit = st.ullTotalPageFile;
    m.total_commit_usage = st.ullTotalPageFile - st.ullAvailPageFile;
    m.app_level = "Desktop";
  }
#endif
  return m;
}

DxgiMemorySnapshot QueryDxgiVideoMemory(IDXGIAdapter3* adapter) {
  DxgiMemorySnapshot snap;
  if (!adapter) return snap;

  auto query_group = [&](DXGI_MEMORY_SEGMENT_GROUP grp, DxgiSegmentInfo& out) {
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    out.hr = adapter->QueryVideoMemoryInfo(0, grp, &info);
    if (SUCCEEDED(out.hr)) {
      out.budget = info.Budget;
      out.current_usage = info.CurrentUsage;
      out.available_for_reservation = info.AvailableForReservation;
      out.current_reservation = info.CurrentReservation;
    }
  };
  query_group(DXGI_MEMORY_SEGMENT_GROUP_LOCAL, snap.local);
  query_group(DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, snap.non_local);
  return snap;
}

std::vector<CpuSetEntry> QueryCpuSets() {
  std::vector<CpuSetEntry> result;
  ULONG needed = 0;
  GetSystemCpuSetInformation(nullptr, 0, &needed, GetCurrentProcess(), 0);
  if (needed == 0) return result;
  std::vector<uint8_t> buf(needed);
  if (!GetSystemCpuSetInformation(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buf.data()),
                                  needed, &needed, GetCurrentProcess(), 0)) {
    return result;
  }
  size_t offset = 0;
  while (offset + sizeof(ULONG) <= needed) {
    auto* info = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buf.data() + offset);
    if (info->Size == 0 || offset + info->Size > needed) break;
    if (info->Type == CpuSetInformation) {
      CpuSetEntry e;
      e.id = info->CpuSet.Id;
      e.group = info->CpuSet.Group;
      e.logical_processor_index = info->CpuSet.LogicalProcessorIndex;
      e.core_index = info->CpuSet.CoreIndex;
      e.last_level_cache_index = info->CpuSet.LastLevelCacheIndex;
      e.numa_node_index = info->CpuSet.NumaNodeIndex;
      e.efficiency_class = info->CpuSet.EfficiencyClass;
      e.scheduling_class = info->CpuSet.SchedulingClass;
      e.all_flags = info->CpuSet.AllFlags;
      e.parked = info->CpuSet.Parked != 0;
      e.allocated = info->CpuSet.Allocated != 0;
      e.allocated_to_target = info->CpuSet.AllocatedToTargetProcess != 0;
      e.real_time = info->CpuSet.RealTime != 0;
      result.push_back(e);
    }
    offset += info->Size;
  }
  return result;
}

JsonValue MemorySnapshotToJson(const MemorySnapshot& m) {
  JsonValue j = JsonValue::Object();
  j.Set("memory_manager_ok", JsonValue::Bool(m.memory_manager_ok));
  j.Set("app_usage_bytes", JsonValue::UInt(m.app_usage));
  j.Set("app_usage_mib", JsonValue::Double(static_cast<double>(m.app_usage) / (1024.0 * 1024.0)));
  j.Set("app_limit_bytes", JsonValue::UInt(m.app_limit));
  j.Set("app_limit_mib", JsonValue::Double(static_cast<double>(m.app_limit) / (1024.0 * 1024.0)));
  j.Set("app_level", JsonValue::String(m.app_level));
  j.Set("private_commit_mib", JsonValue::Double(static_cast<double>(m.private_commit_usage) / (1024.0 * 1024.0)));
  j.Set("total_commit_mib", JsonValue::Double(static_cast<double>(m.total_commit_usage) / (1024.0 * 1024.0)));
  j.Set("total_commit_limit_mib", JsonValue::Double(static_cast<double>(m.total_commit_limit) / (1024.0 * 1024.0)));
  j.Set("expected_total_commit_limit_mib",
        JsonValue::Double(static_cast<double>(m.expected_total_commit_limit) / (1024.0 * 1024.0)));
  j.Set("private_working_set_mib",
        JsonValue::Double(static_cast<double>(m.private_working_set_usage) / (1024.0 * 1024.0)));
  j.Set("ev_usage_increased", JsonValue::UInt(m.ev_usage_increased));
  j.Set("ev_usage_decreased", JsonValue::UInt(m.ev_usage_decreased));
  j.Set("ev_limit_changing", JsonValue::UInt(m.ev_limit_changing));

  JsonValue xp = JsonValue::Object();
  xp.Set("ok", JsonValue::Bool(m.xbox_pools_ok));
  xp.Set("hr", JsonValue::String(FormatHr(m.xbox_pools_hr)));
  if (m.xbox_pools_ok) {
    xp.Set("app_usage_mib", JsonValue::Double(static_cast<double>(m.xbox_app_usage) / (1024.0 * 1024.0)));
    xp.Set("app_limit_mib", JsonValue::Double(static_cast<double>(m.xbox_app_limit) / (1024.0 * 1024.0)));
    xp.Set("extended_usage_mib", JsonValue::Double(static_cast<double>(m.xbox_ext_usage) / (1024.0 * 1024.0)));
    xp.Set("extended_limit_mib", JsonValue::Double(static_cast<double>(m.xbox_ext_limit) / (1024.0 * 1024.0)));
  }
  j.Set("xbox_pools", std::move(xp));
  return j;
}

JsonValue DxgiMemoryToJson(const DxgiMemorySnapshot& d) {
  auto seg = [](const DxgiSegmentInfo& s) {
    JsonValue j = JsonValue::Object();
    j.Set("hr", JsonValue::String(FormatHr(s.hr)));
    j.Set("budget_mib", JsonValue::Double(static_cast<double>(s.budget) / (1024.0 * 1024.0)));
    j.Set("usage_mib", JsonValue::Double(static_cast<double>(s.current_usage) / (1024.0 * 1024.0)));
    j.Set("avail_reservation_mib",
          JsonValue::Double(static_cast<double>(s.available_for_reservation) / (1024.0 * 1024.0)));
    j.Set("current_reservation_mib",
          JsonValue::Double(static_cast<double>(s.current_reservation) / (1024.0 * 1024.0)));
    return j;
  };
  JsonValue j = JsonValue::Object();
  j.Set("local", seg(d.local));
  j.Set("non_local", seg(d.non_local));
  return j;
}

std::string FormatMemoryLine(const MemorySnapshot& m) {
  std::ostringstream os;
  os << "app=" << FormatMib(m.app_usage) << "/" << FormatMib(m.app_limit) << " MiB"
     << " (" << m.app_level << ")"
     << " priv=" << FormatMib(m.private_commit_usage)
     << " ws=" << FormatMib(m.private_working_set_usage);
  if (m.xbox_pools_ok) {
    os << " xbox_pool[app=" << FormatMib(m.xbox_app_usage) << "/" << FormatMib(m.xbox_app_limit)
       << " ext=" << FormatMib(m.xbox_ext_usage) << "/" << FormatMib(m.xbox_ext_limit) << "]";
  }
  return os.str();
}

std::string FormatDxgiLine(const DxgiMemorySnapshot& d) {
  std::ostringstream os;
  os << "LOCAL[";
  if (SUCCEEDED(d.local.hr)) {
    os << "use=" << FormatMib(d.local.current_usage) << " bud=" << FormatMib(d.local.budget)
       << " res=" << FormatMib(d.local.current_reservation) << "/" << FormatMib(d.local.available_for_reservation);
  } else {
    os << "hr=" << FormatHr(d.local.hr);
  }
  os << "] NON_LOCAL[";
  if (SUCCEEDED(d.non_local.hr)) {
    os << "use=" << FormatMib(d.non_local.current_usage) << " bud=" << FormatMib(d.non_local.budget)
       << " res=" << FormatMib(d.non_local.current_reservation) << "/" << FormatMib(d.non_local.available_for_reservation);
  } else {
    os << "hr=" << FormatHr(d.non_local.hr);
  }
  os << "]";
  return os.str();
}

bool CreateAdapter(bool use_warp, Microsoft::WRL::ComPtr<IDXGIFactory4>& factory,
                   Microsoft::WRL::ComPtr<IDXGIAdapter1>& adapter, std::string& err) {
  HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
  if (FAILED(hr)) {
    err = "CreateDXGIFactory2 failed: " + FormatHr(hr);
    return false;
  }
  if (use_warp) {
    Microsoft::WRL::ComPtr<IDXGIAdapter> warp;
    hr = factory->EnumWarpAdapter(IID_PPV_ARGS(&warp));
    if (FAILED(hr)) {
      err = "EnumWarpAdapter failed: " + FormatHr(hr);
      return false;
    }
    hr = warp.As(&adapter);
    if (FAILED(hr)) {
      err = "QI IDXGIAdapter1 on WARP failed: " + FormatHr(hr);
      return false;
    }
    return true;
  }
  for (UINT i = 0;; ++i) {
    Microsoft::WRL::ComPtr<IDXGIAdapter1> cand;
    if (factory->EnumAdapters1(i, &cand) == DXGI_ERROR_NOT_FOUND) break;
    DXGI_ADAPTER_DESC1 desc{};
    cand->GetDesc1(&desc);
    if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
    adapter = cand;
    return true;
  }
  err = "No hardware DXGI adapter found";
  return false;
}

JsonValue CollectSystemInfo(bool use_warp, ProfilerReport& report) {
  JsonValue j = JsonValue::Object();
  PackageMeta pkg = QueryPackageMeta();
  JsonValue jpkg = JsonValue::Object();
  jpkg.Set("identity_name", JsonValue::String(pkg.identity_name));
  jpkg.Set("full_name", JsonValue::String(pkg.full_name));
  jpkg.Set("family_name", JsonValue::String(pkg.family_name));
  jpkg.Set("display_name", JsonValue::String(pkg.display_name));
  jpkg.Set("version", JsonValue::String(pkg.version));
  jpkg.Set("variant_tag", JsonValue::String(pkg.variant_tag));
  jpkg.Set("device_family", JsonValue::String(pkg.device_family));
  jpkg.Set("device_family_version", JsonValue::String(pkg.device_family_version));
  j.Set("package", std::move(jpkg));

  SYSTEM_INFO si{};
  GetNativeSystemInfo(&si);
  int cpu_info[4] = {};
  __cpuid(cpu_info, 0);
  char vendor[13] = {};
  std::memcpy(vendor + 0, &cpu_info[1], 4);
  std::memcpy(vendor + 4, &cpu_info[3], 4);
  std::memcpy(vendor + 8, &cpu_info[2], 4);
  char brand[49] = {};
  int ext_max[4] = {};
  __cpuid(ext_max, static_cast<int>(0x80000000));
  if (static_cast<unsigned>(ext_max[0]) >= 0x80000004u) {
    for (int i = 0; i < 3; ++i) {
      int regs[4] = {};
      __cpuid(regs, static_cast<int>(0x80000002u + i));
      std::memcpy(brand + i * 16, regs, 16);
    }
  }
  int feat1[4] = {}, feat7[4] = {};
  __cpuid(feat1, 1);
  __cpuidex(feat7, 7, 0);
  bool has_avx = (feat1[2] & (1 << 28)) != 0;
  bool has_fma = (feat1[2] & (1 << 12)) != 0;
  bool has_avx2 = (feat7[1] & (1 << 5)) != 0;

  JsonValue jcpu = JsonValue::Object();
  jcpu.Set("vendor", JsonValue::String(vendor));
  jcpu.Set("brand", JsonValue::String(brand));
  jcpu.Set("native_num_processors", JsonValue::UInt(si.dwNumberOfProcessors));
  jcpu.Set("hardware_concurrency", JsonValue::UInt(std::thread::hardware_concurrency()));
  jcpu.Set("page_size", JsonValue::UInt(si.dwPageSize));
  jcpu.Set("avx", JsonValue::Bool(has_avx));
  jcpu.Set("avx2", JsonValue::Bool(has_avx2));
  jcpu.Set("fma", JsonValue::Bool(has_fma));

  auto sets = QueryCpuSets();
  JsonValue jsets = JsonValue::Array();
  uint32_t alloc_to_target = 0;
  for (const auto& s : sets) {
    if (s.allocated_to_target) ++alloc_to_target;
    JsonValue js = JsonValue::Object();
    js.Set("id", JsonValue::UInt(s.id));
    js.Set("group", JsonValue::UInt(s.group));
    js.Set("logical_index", JsonValue::UInt(s.logical_processor_index));
    js.Set("core_index", JsonValue::UInt(s.core_index));
    js.Set("llc_index", JsonValue::UInt(s.last_level_cache_index));
    js.Set("numa_index", JsonValue::UInt(s.numa_node_index));
    js.Set("efficiency_class", JsonValue::UInt(s.efficiency_class));
    js.Set("scheduling_class", JsonValue::UInt(s.scheduling_class));
    js.Set("parked", JsonValue::Bool(s.parked));
    js.Set("allocated", JsonValue::Bool(s.allocated));
    js.Set("allocated_to_target", JsonValue::Bool(s.allocated_to_target));
    js.Set("real_time", JsonValue::Bool(s.real_time));
    jsets.Push(std::move(js));
  }
  jcpu.Set("cpu_sets_count", JsonValue::UInt(sets.size()));
  jcpu.Set("cpu_sets_allocated_to_target", JsonValue::UInt(alloc_to_target));
  jcpu.Set("cpu_sets", std::move(jsets));
  j.Set("cpu", std::move(jcpu));

  MemorySnapshot mem = CaptureMemorySnapshot();
  j.Set("initial_memory", MemorySnapshotToJson(mem));

  report.Log("=== SYSTEM INFO ===");
  report.Log("Package: " + pkg.identity_name + " (" + pkg.display_name + ") v" + pkg.version +
             " [" + pkg.device_family + " " + pkg.device_family_version + "]");
  report.Log("CPU: " + std::string(vendor) + " \"" + std::string(brand) + "\" native_procs=" +
             std::to_string(si.dwNumberOfProcessors) + " hw_conc=" +
             std::to_string(std::thread::hardware_concurrency()) + " cpu_sets=" +
             std::to_string(sets.size()) + " alloc_to_target=" + std::to_string(alloc_to_target) +
             " avx=" + std::to_string(has_avx) + " avx2=" + std::to_string(has_avx2) +
             " fma=" + std::to_string(has_fma));
  report.Log("Initial memory: " + FormatMemoryLine(mem));
  if (!mem.xbox_pools_ok) {
    report.Log("Xbox extended pools: unavailable (hr=" + FormatHr(mem.xbox_pools_hr) + ")");
  }

  Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
  Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
  std::string err;
  JsonValue jgpu = JsonValue::Object();
  if (CreateAdapter(use_warp, factory, adapter, err)) {
    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    std::string adapter_name = WideToUtf8(desc.Description);
    jgpu.Set("description", JsonValue::String(adapter_name));
    jgpu.Set("vendor_id", JsonValue::UInt(desc.VendorId));
    jgpu.Set("device_id", JsonValue::UInt(desc.DeviceId));
    jgpu.Set("dedicated_video_mib",
             JsonValue::Double(static_cast<double>(desc.DedicatedVideoMemory) / (1024.0 * 1024.0)));
    jgpu.Set("dedicated_system_mib",
             JsonValue::Double(static_cast<double>(desc.DedicatedSystemMemory) / (1024.0 * 1024.0)));
    jgpu.Set("shared_system_mib",
             JsonValue::Double(static_cast<double>(desc.SharedSystemMemory) / (1024.0 * 1024.0)));

    Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
    if (SUCCEEDED(adapter.As(&adapter3))) {
      DxgiMemorySnapshot dxgi_mem = QueryDxgiVideoMemory(adapter3.Get());
      jgpu.Set("initial_dxgi_memory", DxgiMemoryToJson(dxgi_mem));
      report.Log("Adapter: " + adapter_name + " (vid=0x" + FormatHr(desc.VendorId).substr(6) +
                 ") ded_vid=" + FormatMib(desc.DedicatedVideoMemory) + " MiB shared=" +
                 FormatMib(desc.SharedSystemMemory) + " MiB");
      report.Log("Initial DXGI memory: " + FormatDxgiLine(dxgi_mem));
    }

    // Probe D3D12 architecture & feature levels on a temporary device, then release it cleanly.
    Microsoft::WRL::ComPtr<ID3D12Device> dev;
    HRESULT hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev));
    jgpu.Set("d3d12_create_hr", JsonValue::String(FormatHr(hr)));
    if (SUCCEEDED(hr)) {
      D3D12_FEATURE_DATA_ARCHITECTURE1 arch{};
      if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE1, &arch, sizeof(arch)))) {
        jgpu.Set("uma", JsonValue::Bool(arch.UMA != FALSE));
        jgpu.Set("cache_coherent_uma", JsonValue::Bool(arch.CacheCoherentUMA != FALSE));
        jgpu.Set("tile_based_renderer", JsonValue::Bool(arch.TileBasedRenderer != FALSE));
        jgpu.Set("isolated_mmu", JsonValue::Bool(arch.IsolatedMMU != FALSE));
      }
      D3D_FEATURE_LEVEL levels[] = {
          D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1,
          D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_2};
      D3D12_FEATURE_DATA_FEATURE_LEVELS fl{};
      fl.NumFeatureLevels = static_cast<UINT>(sizeof(levels) / sizeof(levels[0]));
      fl.pFeatureLevelsRequested = levels;
      if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof(fl)))) {
        char flbuf[16];
        std::snprintf(flbuf, sizeof(flbuf), "%u_%u",
                      static_cast<unsigned>((fl.MaxSupportedFeatureLevel >> 12) & 0xF),
                      static_cast<unsigned>((fl.MaxSupportedFeatureLevel >> 8) & 0xF));
        jgpu.Set("max_feature_level", JsonValue::String(flbuf));
      }
      D3D12_FEATURE_DATA_SHADER_MODEL sm{};
      sm.HighestShaderModel = D3D_SHADER_MODEL_6_6;
      if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm)))) {
        char smbuf[16];
        std::snprintf(smbuf, sizeof(smbuf), "%u_%u",
                      static_cast<unsigned>((sm.HighestShaderModel >> 4) & 0xF),
                      static_cast<unsigned>(sm.HighestShaderModel & 0xF));
        jgpu.Set("highest_shader_model", JsonValue::String(smbuf));
      }
      D3D12_FEATURE_DATA_D3D12_OPTIONS opts{};
      if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opts, sizeof(opts)))) {
        jgpu.Set("resource_heap_tier", JsonValue::Int(static_cast<int>(opts.ResourceHeapTier)));
        jgpu.Set("resource_binding_tier", JsonValue::Int(static_cast<int>(opts.ResourceBindingTier)));
      }
      report.Log("D3D12 arch: UMA=" + std::to_string(arch.UMA != FALSE) +
                 " CacheCoherentUMA=" + std::to_string(arch.CacheCoherentUMA != FALSE) +
                 " IsolatedMMU=" + std::to_string(arch.IsolatedMMU != FALSE));
    }
  } else {
    jgpu.Set("error", JsonValue::String(err));
    report.Log("Adapter error: " + err);
  }
  j.Set("gpu_adapter", std::move(jgpu));

  return j;
}
