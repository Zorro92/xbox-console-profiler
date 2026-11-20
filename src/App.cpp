#include "pch.h"
#include "App.h"
#include "BenchBandwidth.h"
#include "BenchCpu.h"
#include "BenchGpu.h"
#include "BenchMemory.h"
#include "SystemInfo.h"

#include <collection.h>
#include <ppltasks.h>

using namespace XboxConsoleProfiler;
using namespace concurrency;
using namespace Windows::ApplicationModel;
using namespace Windows::ApplicationModel::Activation;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::Foundation;
using namespace Windows::System;
using namespace Windows::UI::Core;
using namespace Windows::UI::Input::Preview::Injection;

[Platform::MTAThread]
int main(Platform::Array<Platform::String^>^) {
  RegisterMemoryManagerEvents();
  auto source = ref new ProfilerApplicationSource();
  CoreApplication::Run(source);
  return 0;
}

IFrameworkView^ ProfilerApplicationSource::CreateView() {
  return ref new App();
}

App::App() {}

void App::Initialize(CoreApplicationView^ applicationView) {
  applicationView->Activated +=
      ref new TypedEventHandler<CoreApplicationView^, IActivatedEventArgs^>(this, &App::OnActivated);
  CoreApplication::Suspending +=
      ref new EventHandler<SuspendingEventArgs^>(this, &App::OnSuspending);
  CoreApplication::Resuming +=
      ref new EventHandler<Platform::Object^>(this, &App::OnResuming);
}

void App::SetWindow(CoreWindow^ window) {
  window_ = window;
  window->VisibilityChanged +=
      ref new TypedEventHandler<CoreWindow^, VisibilityChangedEventArgs^>(this, &App::OnVisibilityChanged);
  window->Closed +=
      ref new TypedEventHandler<CoreWindow^, CoreWindowEventArgs^>(this, &App::OnWindowClosed);
  window->KeyDown +=
      ref new TypedEventHandler<CoreWindow^, KeyEventArgs^>(this, &App::OnKeyDown);
  window->PointerPressed +=
      ref new TypedEventHandler<CoreWindow^, PointerEventArgs^>(this, &App::OnPointerPressed);

  try {
    injector_ = InputInjector::TryCreate();
  } catch (...) {
    injector_ = nullptr;
  }

  // Register UI-thread InputInjector callback for the benchmark worker thread.
  input_.SetInjectorCallback([this]() -> bool { return this->InjectShiftOnUiThread(); });
}

void App::Load(Platform::String^) {
  // Worker starts once the window is activated in Run().
}

bool App::InjectShiftOnUiThread() {
  CoreWindow^ win = window_.Get();
  if (win == nullptr) return false;
  auto dispatcher = win->Dispatcher;
  if (dispatcher == nullptr) return false;

  HANDLE done_evt = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
  if (!done_evt) return false;
  auto ok = std::make_shared<std::atomic<bool>>(false);

  try {
    dispatcher->RunAsync(
        CoreDispatcherPriority::High,
        ref new DispatchedHandler([this, done_evt, ok]() {
          try {
            if (injector_ == nullptr) {
              injector_ = InputInjector::TryCreate();
            }
            if (injector_ != nullptr) {
              auto down = ref new InjectedInputKeyboardInfo();
              down->VirtualKey = static_cast<unsigned short>(VirtualKey::Shift);
              down->KeyOptions = InjectedInputKeyOptions::None;

              auto up = ref new InjectedInputKeyboardInfo();
              up->VirtualKey = static_cast<unsigned short>(VirtualKey::Shift);
              up->KeyOptions = InjectedInputKeyOptions::KeyUp;

              auto keys = ref new Platform::Collections::Vector<InjectedInputKeyboardInfo^>();
              keys->Append(down);
              keys->Append(up);
              injector_->InjectKeyboardInput(keys);
              ok->store(true, std::memory_order_release);
            }
          } catch (...) {
            ok->store(false, std::memory_order_release);
          }
          SetEvent(done_evt);
        }));
    WaitForSingleObjectEx(done_evt, 2000, FALSE);
  } catch (...) {
  }
  CloseHandle(done_evt);
  return ok->load(std::memory_order_acquire);
}

void App::StartWorker() {
  if (worker_started_) return;
  worker_started_ = true;
  worker_ = std::thread([this]() { this->WorkerMain(); });
}

void App::JoinWorker() {
  std::lock_guard<std::mutex> lock(worker_join_mu_);
  if (worker_.joinable()) {
    worker_.join();
  }
}

void App::WorkerMain() {
  PackageMeta pkg = QueryPackageMeta();
  std::string args_dir = pkg.local_state_dir + "\\args";
  std::string results_dir = pkg.local_state_dir + "\\results";
  CreateDirectoryA(args_dir.c_str(), nullptr);
  CreateDirectoryA(results_dir.c_str(), nullptr);

  ProfilerConfig cfg = LoadProfilerConfig(args_dir + "\\config.txt");
  exit_on_done_.store(cfg.exit_on_done, std::memory_order_relaxed);

  report_.Open(results_dir, pkg.variant_tag);
  report_.SetStatus("RUNNING sysinfo");
  report_.Log("================================================================");
  report_.Log("xbox-console-profiler starting");
  report_.Log("Config: " + FormatProfilerConfig(cfg));
  report_.Log("InputInjector available at startup: " + std::string(injector_ != nullptr ? "yes" : "no"));

  JsonValue jcfg = JsonValue::Object();
  jcfg.Set("source", JsonValue::String(cfg.source));
  jcfg.Set("run_active", JsonValue::Bool(cfg.run_active));
  jcfg.Set("run_idle", JsonValue::Bool(cfg.run_idle));
  jcfg.Set("wake_mode", JsonValue::String(cfg.wake_mode));
  jcfg.Set("idle_wait_sec", JsonValue::Int(cfg.idle_wait_sec));
  jcfg.Set("mem_step_mib", JsonValue::UInt(cfg.mem_step_mib));
  jcfg.Set("mem_max_mib", JsonValue::UInt(cfg.mem_max_mib));
  jcfg.Set("mixed_cpu_mib", JsonValue::UInt(cfg.mixed_cpu_mib));
  jcfg.Set("allow_over_limit", JsonValue::Bool(cfg.allow_over_limit));
  report_.SetSection("config", std::move(jcfg));

  report_.SetSection("system_info", CollectSystemInfo(cfg.use_warp, report_));
  report_.FlushJson();

  auto run_phase = [&](const std::string& state_label) {
    JsonValue jphase = JsonValue::Object();
    jphase.Set("state", JsonValue::String(state_label));

    if (state_label == "active") {
      report_.SetStatus("RUNNING active_wake");
      std::string wake = input_.EnsureActive(cfg.wake_mode, cfg.manual_wait_sec, cancel_, report_);
      jphase.Set("wake_method", JsonValue::String(wake));
      report_.Log("=== PHASE: ACTIVE (wake=" + wake + ", ms_since_input=" +
                  std::to_string(static_cast<int>(input_.MsSinceLastInput())) + ") ===");
    } else {
      double waited_ms = input_.WaitForIdle(cfg.idle_wait_sec, cancel_, report_);
      jphase.Set("idle_entry_ms_since_input", JsonValue::Double(waited_ms));
      report_.Log("=== PHASE: IDLE (ms_since_input=" + std::to_string(static_cast<int>(waited_ms)) + ") ===");
    }

    jphase.Set("ms_since_last_input_phase_start", JsonValue::Double(input_.MsSinceLastInput()));

    if (cfg.run_cpu && !cancel_.load(std::memory_order_relaxed)) {
      report_.SetStatus("RUNNING " + state_label + "_cpu");
      jphase.Set("cpu", RunCpuBenchmark(state_label, cfg.wake_mode, input_, cancel_, report_));
      report_.SetSection("phase_" + state_label, jphase);
      report_.FlushJson();
    }
    if (cfg.run_bandwidth && !cancel_.load(std::memory_order_relaxed)) {
      report_.SetStatus("RUNNING " + state_label + "_bandwidth");
      jphase.Set("bandwidth", RunBandwidthBenchmark(state_label, cfg.wake_mode, input_, cancel_, report_));
      report_.SetSection("phase_" + state_label, jphase);
      report_.FlushJson();
    }
    if (cfg.run_gpu && !cancel_.load(std::memory_order_relaxed)) {
      report_.SetStatus("RUNNING " + state_label + "_gpu");
      jphase.Set("gpu", RunGpuBenchmark(state_label, cfg.wake_mode, cfg.use_warp, input_, cancel_, report_));
      report_.SetSection("phase_" + state_label, jphase);
      report_.FlushJson();
    }
    if ((cfg.run_mem_cpu || cfg.run_mem_d3d11 || cfg.run_mem_d3d12 ||
         cfg.run_mem_mixed_d3d11 || cfg.run_mem_mixed_d3d12) &&
        !cancel_.load(std::memory_order_relaxed)) {
      report_.SetStatus("RUNNING " + state_label + "_memory");
      jphase.Set("memory", RunMemorySuite(state_label, cfg, input_, cancel_, report_));
      report_.SetSection("phase_" + state_label, jphase);
      report_.FlushJson();
    }

    jphase.Set("ms_since_last_input_phase_end", JsonValue::Double(input_.MsSinceLastInput()));
    report_.SetSection("phase_" + state_label, std::move(jphase));
    report_.FlushJson();
  };

  if (cfg.run_active && !cancel_.load(std::memory_order_relaxed)) {
    run_phase("active");
  }
  if (cfg.run_idle && !cancel_.load(std::memory_order_relaxed)) {
    run_phase("idle");
  }

  bool cancelled = cancel_.load(std::memory_order_relaxed);
  report_.Log(cancelled ? "=== PROFILER CANCELLED ===" : "=== PROFILER COMPLETED ===");
  report_.SetSection("completed", JsonValue::Bool(!cancelled));
  report_.FlushJson();
  report_.SetStatus(cancelled ? "CANCELLED" : "DONE");
  worker_done_.store(true, std::memory_order_release);
}

void App::Run() {
  StartWorker();
  DWORD last_packet[4] = {};
  bool has_packet[4] = {};

  while (!window_closed_) {
    if (window_visible_) {
      CoreWindow::GetForCurrentThread()->Dispatcher->ProcessEvents(
          CoreProcessEventsOption::ProcessAllIfPresent);

      // Poll XInput controllers so real gamepad input is recorded immediately.
      for (DWORD i = 0; i < 4; ++i) {
        XINPUT_STATE st{};
        if (XInputGetState(i, &st) == ERROR_SUCCESS) {
          if (has_packet[i] && st.dwPacketNumber != last_packet[i]) {
            input_.RecordInput("XInput");
          }
          last_packet[i] = st.dwPacketNumber;
          has_packet[i] = true;
        }
      }

      if (worker_done_.load(std::memory_order_acquire) && exit_on_done_.load(std::memory_order_relaxed)) {
        window_closed_ = true;
        break;
      }
      Sleep(25);
    } else {
      CoreWindow::GetForCurrentThread()->Dispatcher->ProcessEvents(
          CoreProcessEventsOption::ProcessOneAndAllPending);
      if (!worker_done_.load(std::memory_order_acquire)) {
        report_.Log("LIFECYCLE Window not visible: cancelling profiler and exiting cleanly");
        cancel_.store(true, std::memory_order_release);
      }
      window_closed_ = true;
    }
  }

  cancel_.store(true, std::memory_order_release);
  JoinWorker();
}

void App::Uninitialize() {
  cancel_.store(true, std::memory_order_release);
  JoinWorker();
}

void App::OnActivated(CoreApplicationView^, IActivatedEventArgs^) {
  CoreWindow::GetForCurrentThread()->Activate();
}

void App::OnSuspending(Platform::Object^, SuspendingEventArgs^ args) {
  report_.Log("LIFECYCLE Suspending");
  cancel_.store(true, std::memory_order_release);
  SuspendingDeferral^ deferral = args->SuspendingOperation->GetDeferral();
  create_task([this, deferral]() {
    JoinWorker();
    deferral->Complete();
  });
}

void App::OnResuming(Platform::Object^, Platform::Object^) {
  report_.Log("LIFECYCLE Resuming");
}

void App::OnVisibilityChanged(CoreWindow^, VisibilityChangedEventArgs^ args) {
  window_visible_ = args->Visible;
  report_.Log(std::string("LIFECYCLE VisibilityChanged: ") + (args->Visible ? "visible" : "not visible"));
}

void App::OnWindowClosed(CoreWindow^, CoreWindowEventArgs^) {
  report_.Log("LIFECYCLE WindowClosed");
  cancel_.store(true, std::memory_order_release);
  window_closed_ = true;
}

void App::OnKeyDown(CoreWindow^, KeyEventArgs^) {
  input_.RecordInput("CoreWindowKey");
}

void App::OnPointerPressed(CoreWindow^, PointerEventArgs^) {
  input_.RecordInput("CoreWindowPointer");
}
