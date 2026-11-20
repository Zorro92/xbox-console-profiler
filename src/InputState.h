#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

class ProfilerReport;

class InputState {
 public:
  InputState();

  // Optional callback invoked on the UI thread to perform InputInjector Shift press.
  // Returns true if injection succeeded.
  void SetInjectorCallback(std::function<bool()> cb);

  // Record a real user/controller/keyboard input event (updates last_input_qpc).
  void RecordInput(const char* source);

  double MsSinceLastInput() const;
  std::string LastInputSource() const;

  // Attempt to put the console into the active (recently-received-input) state.
  // Returns a human-readable description of the wake method used.
  std::string EnsureActive(const std::string& wake_mode, int manual_wait_sec,
                           const std::atomic<bool>& cancel, ProfilerReport& report);

  // Keep active during a multi-step active phase if >4 seconds have elapsed since last input.
  void KeepAliveIfNeeded(const std::string& wake_mode, const std::atomic<bool>& cancel);

  // Wait until at least `idle_wait_sec` have elapsed without input.
  // Returns actual ms since last input after waiting.
  double WaitForIdle(int idle_wait_sec, const std::atomic<bool>& cancel, ProfilerReport& report);

  static int64_t NowQpc();
  static double QpcToMs(int64_t delta_ticks);

 private:
  std::atomic<int64_t> last_input_qpc_{0};
  mutable std::mutex mu_;
  std::string last_source_ = "launch";
  std::function<bool()> injector_cb_;
};
