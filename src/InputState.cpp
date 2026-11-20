#include "pch.h"
#include "InputState.h"
#include "ProfilerReport.h"

namespace {

int64_t QueryQpcFreq() {
  LARGE_INTEGER f{};
  QueryPerformanceFrequency(&f);
  return f.QuadPart > 0 ? f.QuadPart : 1;
}

} // namespace

InputState::InputState() {
  last_input_qpc_.store(NowQpc(), std::memory_order_relaxed);
}

int64_t InputState::NowQpc() {
  LARGE_INTEGER t{};
  QueryPerformanceCounter(&t);
  return t.QuadPart;
}

double InputState::QpcToMs(int64_t delta_ticks) {
  static const int64_t kFreq = QueryQpcFreq();
  return static_cast<double>(delta_ticks) * 1000.0 / static_cast<double>(kFreq);
}

void InputState::SetInjectorCallback(std::function<bool()> cb) {
  std::lock_guard<std::mutex> lock(mu_);
  injector_cb_ = std::move(cb);
}

void InputState::RecordInput(const char* source) {
  last_input_qpc_.store(NowQpc(), std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(mu_);
  last_source_ = source ? source : "unknown";
}

double InputState::MsSinceLastInput() const {
  int64_t last = last_input_qpc_.load(std::memory_order_relaxed);
  return QpcToMs(NowQpc() - last);
}

std::string InputState::LastInputSource() const {
  std::lock_guard<std::mutex> lock(mu_);
  return last_source_;
}

std::string InputState::EnsureActive(const std::string& wake_mode, int manual_wait_sec,
                                     const std::atomic<bool>& cancel, ProfilerReport& report) {
  if (wake_mode == "none") {
    return "none";
  }

  std::function<bool()> cb;
  {
    std::lock_guard<std::mutex> lock(mu_);
    cb = injector_cb_;
  }

  if ((wake_mode == "auto" || wake_mode == "inject") && cb) {
    if (cb()) {
      RecordInput("InputInjector(Shift)");
      Sleep(60);
      return "InputInjector(Shift)";
    }
    report.Log("INPUT: InputInjector(Shift) unavailable or failed");
  }

  if (wake_mode == "manual" || wake_mode == "auto") {
    int64_t start_input = last_input_qpc_.load(std::memory_order_relaxed);
    if (MsSinceLastInput() < 2000.0 && LastInputSource() != "launch") {
      return "recent_" + LastInputSource();
    }
    report.Log("INPUT: Waiting up to " + std::to_string(manual_wait_sec) +
               " s for controller/keyboard input to wake console...");
    report.SetStatus("WAITING_FOR_INPUT");
    int64_t start_qpc = NowQpc();
    while (!cancel.load(std::memory_order_relaxed)) {
      if (last_input_qpc_.load(std::memory_order_relaxed) != start_input) {
        Sleep(60);
        return "manual_" + LastInputSource();
      }
      if (QpcToMs(NowQpc() - start_qpc) >= manual_wait_sec * 1000.0) {
        break;
      }
      Sleep(50);
    }
    report.Log("INPUT: No manual input received before timeout; proceeding with current state");
    return "timeout_no_input";
  }

  return "unavailable";
}

void InputState::KeepAliveIfNeeded(const std::string& wake_mode, const std::atomic<bool>& cancel) {
  if (cancel.load(std::memory_order_relaxed)) return;
  if (wake_mode != "auto" && wake_mode != "inject") return;
  if (MsSinceLastInput() < 4000.0) return;

  std::function<bool()> cb;
  {
    std::lock_guard<std::mutex> lock(mu_);
    cb = injector_cb_;
  }
  if (cb && cb()) {
    RecordInput("InputInjector(Shift)");
    Sleep(30);
  }
}

double InputState::WaitForIdle(int idle_wait_sec, const std::atomic<bool>& cancel, ProfilerReport& report) {
  if (idle_wait_sec <= 0) return MsSinceLastInput();
  report.Log("INPUT: Waiting until >=" + std::to_string(idle_wait_sec) + " s with no input for idle phase...");
  report.SetStatus("WAITING_FOR_IDLE");

  const double target_ms = static_cast<double>(idle_wait_sec) * 1000.0;
  // Cap total wait at 2 * idle_wait_sec + 5 s in case noisy input keeps resetting the timer.
  const int64_t wait_start = NowQpc();
  const double max_wait_ms = (idle_wait_sec * 2 + 5) * 1000.0;

  while (!cancel.load(std::memory_order_relaxed)) {
    double since = MsSinceLastInput();
    if (since >= target_ms) {
      break;
    }
    if (QpcToMs(NowQpc() - wait_start) >= max_wait_ms) {
      report.Log("INPUT: Idle wait cap reached (ms_since_last_input=" +
                 std::to_string(static_cast<int>(since)) + ")");
      break;
    }
    Sleep(100);
  }
  double final_since = MsSinceLastInput();
  char buf[128];
  std::snprintf(buf, sizeof(buf), "INPUT: Entering idle phase (ms_since_last_input=%.0f ms, last=%s)",
                final_since, LastInputSource().c_str());
  report.Log(buf);
  return final_since;
}
