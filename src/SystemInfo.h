#pragma once

#include "ProfilerReport.h"
#include <cstdint>
#include <string>
#include <vector>

struct MemorySnapshot {
  bool memory_manager_ok = false;
  uint64_t app_usage = 0;
  uint64_t app_limit = 0;
  std::string app_level = "unknown";
  uint64_t private_commit_usage = 0;
  uint64_t total_commit_usage = 0;
  uint64_t total_commit_limit = 0;
  uint64_t expected_total_commit_limit = 0;
  uint64_t private_working_set_usage = 0;

  uint64_t ev_usage_increased = 0;
  uint64_t ev_usage_decreased = 0;
  uint64_t ev_limit_changing = 0;

  bool xbox_pools_ok = false;
  HRESULT xbox_pools_hr = E_NOTIMPL;
  uint64_t xbox_app_usage = 0;
  uint64_t xbox_app_limit = 0;
  uint64_t xbox_ext_usage = 0;
  uint64_t xbox_ext_limit = 0;
};

struct DxgiSegmentInfo {
  HRESULT hr = E_NOTIMPL;
  uint64_t budget = 0;
  uint64_t current_usage = 0;
  uint64_t available_for_reservation = 0;
  uint64_t current_reservation = 0;
};

struct DxgiMemorySnapshot {
  DxgiSegmentInfo local;
  DxgiSegmentInfo non_local;
};

struct CpuSetEntry {
  uint32_t id = 0;
  uint16_t group = 0;
  uint8_t logical_processor_index = 0;
  uint8_t core_index = 0;
  uint8_t last_level_cache_index = 0;
  uint8_t numa_node_index = 0;
  uint8_t efficiency_class = 0;
  uint8_t scheduling_class = 0;
  uint8_t all_flags = 0;
  bool parked = false;
  bool allocated = false;
  bool allocated_to_target = false;
  bool real_time = false;
};

struct PackageMeta {
  std::string identity_name = "Desktop";
  std::string full_name = "Desktop";
  std::string family_name = "Desktop";
  std::string display_name = "Desktop";
  std::string version = "0.0.0.0";
  std::string variant_tag = "desktop";
  std::string device_family = "Windows.Desktop";
  std::string device_family_version = "0.0.0.0";
  std::string local_state_dir;
};

void RegisterMemoryManagerEvents();
PackageMeta QueryPackageMeta();
MemorySnapshot CaptureMemorySnapshot();
DxgiMemorySnapshot QueryDxgiVideoMemory(IDXGIAdapter3* adapter);
std::vector<CpuSetEntry> QueryCpuSets();

JsonValue MemorySnapshotToJson(const MemorySnapshot& m);
JsonValue DxgiMemoryToJson(const DxgiMemorySnapshot& d);
std::string FormatMemoryLine(const MemorySnapshot& m);
std::string FormatDxgiLine(const DxgiMemorySnapshot& d);

bool CreateAdapter(bool use_warp, Microsoft::WRL::ComPtr<IDXGIFactory4>& factory,
                   Microsoft::WRL::ComPtr<IDXGIAdapter1>& adapter, std::string& err);

JsonValue CollectSystemInfo(bool use_warp, ProfilerReport& report);
