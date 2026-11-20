#pragma once

#include "InputState.h"
#include "ProfilerReport.h"
#include <atomic>
#include <string>

JsonValue RunBandwidthBenchmark(const std::string& state_label, const std::string& wake_mode,
                                InputState& input, const std::atomic<bool>& cancel,
                                ProfilerReport& report);
