#include "menu_exit.h"
#include <rex/ppc/context.h>
#include <cstdlib>
#include <thread>

namespace {

void Require(bool condition) {
  if (!condition) std::abort();
}

}  // namespace

int main() {
  PPCRegister path{}, flags{};
  path.u32 = 0x820076A9;
  Require(!NbaConsumeMenuExit());
  NbaMenuExitLaunch(path, flags);
  Require(!NbaConsumeMenuExit());
  NbaMenuExitRequested();
  Require(!NbaConsumeMenuExit());
  NbaMenuExitLaunch(path, flags);
  Require(!NbaConsumeMenuExit());
  NbaMenuExitRequested();
  flags.u32 = 1;
  NbaMenuExitLaunch(path, flags);
  flags.u32 = 0;
  NbaMenuExitLaunch(path, flags);
  Require(!NbaConsumeMenuExit());
  NbaMenuExitRequested();
  ++path.u32;
  NbaMenuExitLaunch(path, flags);
  --path.u32;
  NbaMenuExitLaunch(path, flags);
  Require(!NbaConsumeMenuExit());
  NbaMenuExitRequested();
  NbaMenuExitLaunch(path, flags);
  NbaMenuExitRequested();
  Require(NbaConsumeMenuExit());
  Require(!NbaConsumeMenuExit());
  NbaMenuExitRequested();
  NbaMenuExitLaunch(path, flags);
  NbaResetMenuExit();
  Require(!NbaConsumeMenuExit());
  std::thread guest([&]() {
    NbaMenuExitRequested();
    NbaMenuExitLaunch(path, flags);
  });
  guest.join();
  Require(NbaConsumeMenuExit());
}
