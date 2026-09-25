// NBA JAM: On Fire Edition - the controller, straight from Windows
//
// The mod chooser cannot use the pad the game is given. The game asks the
// kernel for controller state with a flag the runtime does not accept, so the
// answer is always "nothing plugged in" and the buffer it would have filled
// stays empty - which is why every other button in the front end works and
// the chooser's did not. The rest of the front end reads the pad another way.
//
// Asking Windows is the way round that, and it costs one file of its own
// because Windows' own xinput.h defines names that collide with the runtime's
// Xbox 360 input headers. Nothing else here includes both.

#include "host_pad.h"

#include <windows.h>
#include <xinput.h>

#pragma comment(lib, "xinput.lib")

bool NbaHostPad(uint16_t* buttons, uint8_t* left_trigger) {
  // Slots with nothing in them are swept only now and then: asking about an
  // empty one is slow, and the answer rarely changes.
  static DWORD which = 0xFFFFFFFF;
  static ULONGLONG last_sweep = 0;

  XINPUT_STATE state{};
  if (which != 0xFFFFFFFF) {
    if (XInputGetState(which, &state) == ERROR_SUCCESS) {
      *buttons = state.Gamepad.wButtons;
      *left_trigger = state.Gamepad.bLeftTrigger;
      return true;
    }
    which = 0xFFFFFFFF;
  }
  const ULONGLONG now = GetTickCount64();
  if (now - last_sweep < 2000) {
    return false;
  }
  last_sweep = now;
  for (DWORD i = 0; i < 4; ++i) {
    if (XInputGetState(i, &state) != ERROR_SUCCESS) {
      continue;
    }
    which = i;
    *buttons = state.Gamepad.wButtons;
    *left_trigger = state.Gamepad.bLeftTrigger;
    return true;
  }
  return false;
}
