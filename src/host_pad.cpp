// NBA JAM: On Fire Edition - the controller, straight from the host
//
// The mod chooser cannot use the pad the game is given. The game asks the
// kernel for controller state with a flag the runtime does not accept, so the
// answer is always "nothing plugged in" and the buffer it would have filled
// stays empty - which is why every other button in the front end works and
// the chooser's did not. The rest of the front end reads the pad another way.
//
// Asking the host is the way round that. On Windows that is XInput, and it
// costs one file of its own because Windows' own xinput.h defines names that
// collide with the runtime's Xbox 360 input headers; nothing else here
// includes both. On Linux it is SDL, which the runtime has already started
// for its own input; its buttons are mapped onto XInput's bits so the chooser
// reads one layout either way.

#include "host_pad.h"

#include "host.h"

#ifdef _WIN32

#include <windows.h>
#include <xinput.h>

#pragma comment(lib, "xinput.lib")

bool NbaHostPad(uint16_t* buttons, uint8_t* left_trigger) {
  // Slots with nothing in them are swept only now and then: asking about an
  // empty one is slow, and the answer rarely changes.
  static DWORD which = 0xFFFFFFFF;
  static uint64_t last_sweep = 0;

  XINPUT_STATE state{};
  if (which != 0xFFFFFFFF) {
    if (XInputGetState(which, &state) == ERROR_SUCCESS) {
      *buttons = state.Gamepad.wButtons;
      *left_trigger = state.Gamepad.bLeftTrigger;
      return true;
    }
    which = 0xFFFFFFFF;
  }
  const uint64_t now = NbaNowMs();
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

#else

#include <SDL3/SDL_gamepad.h>

bool NbaHostPad(uint16_t* buttons, uint8_t* left_trigger) {
  static SDL_Gamepad* pad = nullptr;
  static uint64_t last_sweep = 0;
  if (pad && !SDL_GamepadConnected(pad)) {
    SDL_CloseGamepad(pad);
    pad = nullptr;
  }
  if (!pad) {
    const uint64_t now = NbaNowMs();
    if (now - last_sweep < 2000) return false;
    last_sweep = now;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (ids && count > 0) pad = SDL_OpenGamepad(ids[0]);
    SDL_free(ids);
    if (!pad) return false;
  }
  // XInput's bits, from SDL's names for the same buttons.
  static const struct {
    SDL_GamepadButton sdl;
    uint16_t xinput;
  } kMap[] = {
      {SDL_GAMEPAD_BUTTON_DPAD_UP, 0x0001},     {SDL_GAMEPAD_BUTTON_DPAD_DOWN, 0x0002},
      {SDL_GAMEPAD_BUTTON_DPAD_LEFT, 0x0004},   {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0x0008},
      {SDL_GAMEPAD_BUTTON_START, 0x0010},       {SDL_GAMEPAD_BUTTON_BACK, 0x0020},
      {SDL_GAMEPAD_BUTTON_LEFT_STICK, 0x0040},  {SDL_GAMEPAD_BUTTON_RIGHT_STICK, 0x0080},
      {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0x0100},
      {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0x0200},
      {SDL_GAMEPAD_BUTTON_SOUTH, 0x1000},       {SDL_GAMEPAD_BUTTON_EAST, 0x2000},
      {SDL_GAMEPAD_BUTTON_WEST, 0x4000},        {SDL_GAMEPAD_BUTTON_NORTH, 0x8000},
  };
  uint16_t b = 0;
  for (const auto& m : kMap) {
    if (SDL_GetGamepadButton(pad, m.sdl)) b |= m.xinput;
  }
  *buttons = b;
  const int lt = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
  *left_trigger = uint8_t(lt <= 0 ? 0 : lt * 255 / 32767);
  return true;
}

#endif
