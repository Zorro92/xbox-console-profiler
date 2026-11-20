#pragma once

#include <cstdint>
#include <string>

struct ProfilerConfig {
  bool run_sysinfo = true;
  bool run_cpu = true;
  bool run_bandwidth = true;
  bool run_gpu = true;
  bool run_mem_cpu = true;
  bool run_mem_d3d11 = true;
  bool run_mem_d3d12 = true;
  bool run_mem_mixed_d3d11 = true;
  bool run_mem_mixed_d3d12 = true;

  bool run_active = true;
  bool run_idle = true;

  int idle_wait_sec = 15;       // >12 s to ensure console drops into input-idle state
  int manual_wait_sec = 20;     // fallback wait for manual button press if InputInjector unavailable
  std::string wake_mode = "auto"; // "auto" | "inject" | "manual" | "none"

  uint32_t mem_step_mib = 100;
  uint32_t mem_max_mib = 16384;
  uint32_t mixed_cpu_mib = 0;   // 0 = auto: min(1000, 40% of AppMemoryUsageLimit)
  bool allow_over_limit = false;

  bool use_warp = false;        // true only in desktop WARP smoke test
  bool exit_on_done = true;

  std::string source = "defaults";
};

ProfilerConfig LoadProfilerConfig(const std::string& config_path);
std::string FormatProfilerConfig(const ProfilerConfig& cfg);
