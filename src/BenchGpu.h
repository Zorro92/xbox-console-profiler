#pragma once

#include "InputState.h"
#include "ProfilerReport.h"
#include <atomic>
#include <string>

JsonValue RunGpuBenchmark(const std::string& state_label, const std::string& wake_mode,
                          bool use_warp, InputState& input, const std::atomic<bool>& cancel,
                          ProfilerReport& report);
