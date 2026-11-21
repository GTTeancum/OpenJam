// NBA JAM: On Fire Edition - intentional in-game exit.
#pragma once

#include <memory>

namespace rex::ppc {
union Register;
}

// These mid-ASM hooks only record the guest's original exit flow.
void NbaMenuExitRequested();
void NbaMenuExitLaunch(rex::ppc::Register& path, rex::ppc::Register& flags);

void NbaResetMenuExit();
bool NbaConsumeMenuExit();

#if defined(__linux__)
namespace rex::ui {
class Window;
class WindowedAppContext;
}

// Only the UI thread reads or invalidates the native window pointer. The
// queued close may outlive the app if the SDL event loop exits with an error.
struct NbaMenuExitWindow {
  rex::ui::Window* window = nullptr;
};

void NbaQueueMenuExitClose(rex::ui::WindowedAppContext& context,
                           const std::shared_ptr<NbaMenuExitWindow>& target);
#endif
