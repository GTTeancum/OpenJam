#include "menu_exit.h"

#include <atomic>
#include <cstdint>
#include <rex/ppc/context.h>
#if defined(__linux__)
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>
#endif

namespace {

enum class ExitState { kIdle, kRequested, kCommitted };
std::atomic<ExitState> g_menu_exit{ExitState::kIdle};

#if defined(__linux__)
// The stock XEX passes a pointer to an empty string on its way out.
// Ordinary post-main cleanup uses this same call; it is not an exit marker
// without an earlier RequestGameExit.
constexpr uint32_t kEmptyLaunchPath = 0x820076A9;
#endif

}  // namespace

void NbaMenuExitRequested() {
#if defined(__linux__)
  auto expected = ExitState::kIdle;
  g_menu_exit.compare_exchange_strong(expected, ExitState::kRequested);
#endif
}

void NbaMenuExitLaunch(PPCRegister& path, PPCRegister& flags) {
#if defined(__linux__)
  auto expected = ExitState::kRequested;
  const auto next = path.u32 == kEmptyLaunchPath && flags.u32 == 0
                        ? ExitState::kCommitted
                        : ExitState::kIdle;
  g_menu_exit.compare_exchange_strong(expected, next);
#else
  (void)path;
  (void)flags;
#endif
}

void NbaResetMenuExit() {
  g_menu_exit.store(ExitState::kIdle);
}

bool NbaConsumeMenuExit() {
  return g_menu_exit.exchange(ExitState::kIdle) == ExitState::kCommitted;
}

#if defined(__linux__)
void NbaQueueMenuExitClose(rex::ui::WindowedAppContext& context,
                           const std::shared_ptr<NbaMenuExitWindow>& target) {
  if (!NbaConsumeMenuExit()) {
    return;
  }
  // The guest already ran its cleanup. A plain SDK UI quit can leave a
  // native Linux process behind. Queue normal window close ahead of that
  // quit without changing mod relaunch or other guest exits.
  context.CallInUIThreadDeferred([target]() {
    if (auto* window = target->window) {
      window->RequestClose();
    }
  });
}
#endif
