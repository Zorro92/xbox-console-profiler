#pragma once

#include "InputState.h"
#include "ProfilerConfig.h"
#include "ProfilerReport.h"

#include <atomic>
#include <memory>
#include <thread>

namespace XboxConsoleProfiler {

ref class App sealed : public Windows::ApplicationModel::Core::IFrameworkView {
 public:
  App();

  virtual void Initialize(Windows::ApplicationModel::Core::CoreApplicationView^ applicationView);
  virtual void SetWindow(Windows::UI::Core::CoreWindow^ window);
  virtual void Load(Platform::String^ entryPoint);
  virtual void Run();
  virtual void Uninitialize();

 protected:
  void OnActivated(Windows::ApplicationModel::Core::CoreApplicationView^ applicationView,
                   Windows::ApplicationModel::Activation::IActivatedEventArgs^ args);
  void OnSuspending(Platform::Object^ sender, Windows::ApplicationModel::SuspendingEventArgs^ args);
  void OnResuming(Platform::Object^ sender, Platform::Object^ args);

  void OnVisibilityChanged(Windows::UI::Core::CoreWindow^ sender,
                           Windows::UI::Core::VisibilityChangedEventArgs^ args);
  void OnWindowClosed(Windows::UI::Core::CoreWindow^ sender,
                      Windows::UI::Core::CoreWindowEventArgs^ args);
  void OnKeyDown(Windows::UI::Core::CoreWindow^ sender, Windows::UI::Core::KeyEventArgs^ args);
  void OnPointerPressed(Windows::UI::Core::CoreWindow^ sender,
                        Windows::UI::Core::PointerEventArgs^ args);

 private:
  void StartWorker();
  void JoinWorker();
  void WorkerMain();
  bool InjectShiftOnUiThread();

  Platform::Agile<Windows::UI::Core::CoreWindow> window_;
  Windows::UI::Input::Preview::Injection::InputInjector^ injector_;

  bool window_closed_ = false;
  bool window_visible_ = true;
  bool worker_started_ = false;

  std::atomic<bool> cancel_{false};
  std::atomic<bool> worker_done_{false};
  std::atomic<bool> exit_on_done_{true};

  InputState input_;
  ProfilerReport report_;
  std::mutex worker_join_mu_;
  std::thread worker_;
};

ref class ProfilerApplicationSource sealed
    : Windows::ApplicationModel::Core::IFrameworkViewSource {
 public:
  virtual Windows::ApplicationModel::Core::IFrameworkView^ CreateView();
};

} // namespace XboxConsoleProfiler
