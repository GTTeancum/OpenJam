#include "host_pad.h"

#include <algorithm>
#include <utility>

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_init.h>

namespace {
NbaHostPadState Read(uint32_t id, SDL_Gamepad* pad) {
  NbaHostPadState state;
  state.id = id;
  if (const char* name = SDL_GetGamepadName(pad)) state.name = name;
  static constexpr struct {
    SDL_GamepadButton sdl;
    uint16_t xbox;
  } buttons[] = {
      {SDL_GAMEPAD_BUTTON_DPAD_UP, 0x0001},
      {SDL_GAMEPAD_BUTTON_DPAD_DOWN, 0x0002},
      {SDL_GAMEPAD_BUTTON_DPAD_LEFT, 0x0004},
      {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0x0008},
      {SDL_GAMEPAD_BUTTON_START, 0x0010},
      {SDL_GAMEPAD_BUTTON_BACK, 0x0020},
      {SDL_GAMEPAD_BUTTON_LEFT_STICK, 0x0040},
      {SDL_GAMEPAD_BUTTON_RIGHT_STICK, 0x0080},
      {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0x0100},
      {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0x0200},
      {SDL_GAMEPAD_BUTTON_SOUTH, 0x1000},
      {SDL_GAMEPAD_BUTTON_EAST, 0x2000},
      {SDL_GAMEPAD_BUTTON_WEST, 0x4000},
      {SDL_GAMEPAD_BUTTON_NORTH, 0x8000},
  };
  for (const auto& button : buttons) {
    if (SDL_GetGamepadButton(pad, button.sdl)) state.buttons |= button.xbox;
  }
  // SDL normalizes triggers to 0..32767 on Xbox, PlayStation and Switch pads.
  const int trigger = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
  state.left_trigger = uint8_t((std::max)(0, trigger) * 255 / 32767);
  return state;
}
}  // namespace

NbaHostPads::NbaHostPads(std::filesystem::path mappings)
    : mappings_(std::move(mappings)) {}

NbaHostPads::~NbaHostPads() {
  for (const auto& pad : pads_) SDL_CloseGamepad(pad.handle);
  if (initialized_) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}

bool NbaHostPads::Poll(NbaHostPadState& state, bool captured) {
  state = {};
  if (!attempted_) {
    attempted_ = true;
    // Own a reference to the subsystem. In a static-SDL Windows SDK, the
    // executable and runtime DLL need not share SDL's initialization state.
    owns_events_ = SDL_WasInit(SDL_INIT_EVENTS) == 0;
    initialized_ = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    if (!initialized_) {
      error_ = std::string("SDL gamepad initialization failed: ") + SDL_GetError();
    } else {
      std::error_code ec;
      if (!mappings_.empty() && std::filesystem::exists(mappings_, ec)) {
        const auto utf8 = mappings_.u8string();
        if (SDL_AddGamepadMappingsFromFile(
                reinterpret_cast<const char*>(utf8.c_str())) < 0) {
          error_ = std::string("SDL gamepad mappings could not be loaded: ") + SDL_GetError();
        }
      }
    }
  }
  if (!initialized_) return false;

  // Do not depend on the guest polling (it is deliberately muted while the
  // chooser is open), or on another SDL instance pumping our event queue.
  SDL_UpdateGamepads();
  std::erase_if(pads_, [](const Pad& pad) {
    if (SDL_GamepadConnected(pad.handle)) return false;
    SDL_CloseGamepad(pad.handle);
    return true;
  });
  int count = 0;
  SDL_JoystickID* ids = SDL_GetGamepads(&count);
  for (int i = 0; ids && i < count; ++i) {
    const auto id = ids[i];
    if (std::none_of(pads_.begin(), pads_.end(),
                     [id](const Pad& pad) { return pad.id == id; })) {
      if (auto* handle = SDL_OpenGamepad(id)) pads_.push_back({id, handle});
    }
  }
  SDL_free(ids);
  if (owns_events_) {
    // A private static-SDL instance has no UI event loop draining its queue.
    // We poll state directly; discard only its controller events. When sharing
    // the runtime's SDL instance, leave its event queue alone.
    SDL_FlushEvents(SDL_EVENT_JOYSTICK_AXIS_MOTION, SDL_EVENT_GAMEPAD_STEAM_HANDLE_UPDATED);
  }

  auto selected = std::find_if(pads_.begin(), pads_.end(),
      [this](const Pad& pad) { return pad.id == selected_; });
  if (captured) {
    if (selected == pads_.end()) {
      selected_ = 0;
      return false;
    }
  } else {
    // Prefer the pad actually requesting the chooser, even with an idle
    // controller in an earlier slot. Never combine buttons from different pads.
    auto active = pads_.end();
    for (auto it = pads_.begin(); it != pads_.end(); ++it) {
      const auto input = Read(it->id, it->handle);
      if (input.left_trigger >= 64 && (input.buttons & 0x8000)) {
        active = it;
        break;
      }
      // The recovery dialog also uses this reader for A/B. Otherwise prefer
      // whichever pad is pressing a button over an idle earlier slot.
      if (active == pads_.end() && input.buttons) active = it;
    }
    if (active != pads_.end()) selected = active;
    if (selected == pads_.end() && !pads_.empty()) selected = pads_.begin();
  }
  if (selected == pads_.end()) {
    selected_ = 0;
    return false;
  }
  selected_ = selected->id;
  state = Read(selected->id, selected->handle);
  return true;
}
