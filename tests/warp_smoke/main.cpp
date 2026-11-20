// Desktop WARP smoke test exercising the shared profiler benchmark modules in CI.
#include "pch.h"
#include "BenchBandwidth.h"
#include "BenchCpu.h"
#include "BenchGpu.h"
#include "BenchMemory.h"
#include "InputState.h"
#include "ProfilerConfig.h"
#include "ProfilerReport.h"
#include "SystemInfo.h"

int main(int argc, char** argv) {
  std::string out_dir = (argc > 1 && argv[1][0] != '\0') ? argv[1] : "warp_smoke_out";
  CreateDirectoryA(out_dir.c_str(), nullptr);

  ProfilerConfig cfg;
  cfg.use_warp = true;
  cfg.wake_mode = "none";
  cfg.idle_wait_sec = 0;
  cfg.mem_step_mib = 64;
  cfg.mem_max_mib = 256;
  cfg.mixed_cpu_mib = 64;
  cfg.run_idle = false;

  ProfilerReport report;
  if (!report.Open(out_dir, "warp_smoke")) {
    std::fprintf(stderr, "Failed to open report in %s\n", out_dir.c_str());
    return 1;
  }

  InputState input;
  std::atomic<bool> cancel{false};

  report.SetSection("system_info", CollectSystemInfo(true, report));
  JsonValue phase = JsonValue::Object();
  phase.Set("cpu", RunCpuBenchmark("active", "none", input, cancel, report));
  phase.Set("bandwidth", RunBandwidthBenchmark("active", "none", input, cancel, report));
  phase.Set("gpu", RunGpuBenchmark("active", "none", true, input, cancel, report));
  phase.Set("memory", RunMemorySuite("active", cfg, input, cancel, report));
  report.SetSection("phase_active", std::move(phase));
  report.SetSection("completed", JsonValue::Bool(true));
  report.FlushJson();
  report.SetStatus("DONE");

  std::printf("WARP smoke test succeeded. JSON: %s\n", report.JsonPath().c_str());
  return 0;
}
