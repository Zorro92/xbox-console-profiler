#pragma once

#include "InputState.h"
#include "ProfilerConfig.h"
#include "ProfilerReport.h"
#include <atomic>
#include <string>

JsonValue RunMemorySuite(const std::string& state_label, const ProfilerConfig& cfg,
                         InputState& input, const std::atomic<bool>& cancel,
                         ProfilerReport& report);
