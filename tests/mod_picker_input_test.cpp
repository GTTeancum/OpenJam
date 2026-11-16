// SDL virtual devices live only in this test process. No window or OS input.
#include "host.h"
#include "host_pad.h"
#include "mod_picker.h"
#include "mod_swap.h"

#include <iostream>
#include <stdexcept>
#include <SDL3/SDL.h>
#include <rex/input/input_system.h>

namespace {
using rex::X_RESULT;
using rex::X_STATUS;
bool main_menu = true;
std::string loaded;
void Require(bool value, const char* message) {
  if (!value) throw std::runtime_error(std::string(message) + ": " + SDL_GetError());
}

struct VirtualPad {
  SDL_JoystickID id = 0;
  SDL_Joystick* joystick = nullptr;
  explicit VirtualPad(const char* name) {
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.name = name;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
    desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
    id = SDL_AttachVirtualJoystick(&desc);
    Require(id != 0, "Attach virtual controller");
    joystick = SDL_OpenJoystick(id);
    Require(joystick != nullptr, "Open virtual controller");
    Neutral();
  }
  ~VirtualPad() { Disconnect(); }
  void Disconnect() {
    if (joystick) SDL_CloseJoystick(joystick);
    joystick = nullptr;
    if (id) SDL_DetachVirtualJoystick(id);
    id = 0;
  }
  void Button(SDL_GamepadButton button, bool down) {
    Require(SDL_SetJoystickVirtualButton(joystick, int(button), down), "Set virtual button");
  }
  void Trigger(Sint16 raw) {
    Require(SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, raw),
            "Set virtual trigger");
  }
  void Neutral() {
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) Button(SDL_GamepadButton(b), false);
    Trigger(SDL_JOYSTICK_AXIS_MIN);
    Require(SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
                                      SDL_JOYSTICK_AXIS_MIN), "Release right trigger");
  }
};

// Observe the real picker callback that mutes guest input while it is open.
class GuestDriver final : public rex::input::InputDriver {
 public:
  GuestDriver() : InputDriver(nullptr, 0) {}
  bool Active() const { return is_active(); }
  X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  void EnumerateDevices(std::vector<rex::input::DeviceInfo>&) override {}
  X_RESULT GetDeviceState(rex::input::DeviceId, rex::input::X_INPUT_STATE*) override {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  X_RESULT GetDeviceCapabilities(rex::input::DeviceId, uint32_t,
                                 rex::input::X_INPUT_CAPABILITIES*) override {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  X_RESULT SetDeviceVibration(rex::input::DeviceId, rex::input::X_INPUT_VIBRATION*) override {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  X_RESULT GetDeviceKeystroke(rex::input::DeviceId, uint32_t,
                             rex::input::X_INPUT_KEYSTROKE*) override {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
};
}  // namespace

// Replace only the game's screen detector and mod-launch side effects.
bool NbaFrontEndOnMainMenu() { return main_menu; }
bool NbaEnvOn(const char*) { return false; }
std::filesystem::path NbaExeDir() { return {}; }
std::filesystem::path NbaPathFromUtf8(const std::string& path) {
  return std::filesystem::path(std::u8string_view(
      reinterpret_cast<const char8_t*>(path.data()), path.size()));
}
bool NbaModList(std::vector<NbaMod>* mods, size_t* active) {
  *mods = {{"base", "base", "Base"}, {"test", "test", "Test Mod"}};
  *active = 0;
  return true;
}
void NbaModSwapTo(const NbaMod& mod) { loaded = mod.id; }

int main() {
  try {
    // Exclude real hardware from the fixture without installing a virtual OS
    // device or changing any system settings. These hints are process-local.
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_DIRECTINPUT, "0");
    SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "0");
    NbaHostPads pads({});
    NbaHostPadState state;
    Require(!pads.Poll(state, false), "No controller must report neutral/disconnected");
    Require(pads.error().empty(), "SDL reader initialization");
    Require(state.buttons == 0 && state.left_trigger == 0, "Disconnected input must be zero");
    VirtualPad first("Idle virtual Xbox pad");
    VirtualPad second("Virtual PlayStation layout");
    Require(pads.Poll(state, false) && state.id == first.id, "First controller discovery");
    Require(!SDL_HasEvents(SDL_EVENT_JOYSTICK_AXIS_MOTION,
                           SDL_EVENT_GAMEPAD_STEAM_HANDLE_UPDATED),
            "Private SDL controller events must not accumulate without an event loop");

    // Buttons on separate devices must not create an LT+Y chord.
    first.Trigger(SDL_JOYSTICK_AXIS_MAX);
    second.Button(SDL_GAMEPAD_BUTTON_NORTH, true);
    Require(pads.Poll(state, false) &&
            (!(state.buttons & 0x8000) || state.left_trigger < 64), "Do not merge controllers");
    first.Neutral();
    second.Trigger(SDL_JOYSTICK_AXIS_MAX);
    Require(pads.Poll(state, false) && state.id == second.id, "Second pad must open the chooser");
    Require(state.buttons == 0x8000 && state.left_trigger == 255, "L2/Triangle maps to LT/Y");

    rex::input::InputSystem input(nullptr);
    auto driver = std::make_unique<GuestDriver>();
    auto* guest = driver.get();
    input.AddDriver(std::move(driver));
    NbaModPickerUseInput(&input);
    Require(NbaModPickerFeed(state.buttons, state.left_trigger) && !guest->Active(),
            "LT+Y opens chooser and mutes guest input");
    NbaModPickerFeed(state.buttons, state.left_trigger);
    Require(!guest->Active(), "Holding combo must not repeatedly toggle");

    first.Trigger(SDL_JOYSTICK_AXIS_MAX);
    first.Button(SDL_GAMEPAD_BUTTON_NORTH, true);
    second.Neutral();
    Require(pads.Poll(state, true) && state.id == second.id && state.buttons == 0,
            "Other controller cannot steal an open chooser");
    NbaModPickerFeed(state.buttons, state.left_trigger);
    Require(!guest->Active(), "Releasing trigger must keep chooser open");
    second.Button(SDL_GAMEPAD_BUTTON_DPAD_DOWN, true);
    pads.Poll(state, true);
    NbaModPickerFeed(state.buttons, state.left_trigger);
    second.Neutral();
    pads.Poll(state, true);
    NbaModPickerFeed(state.buttons, state.left_trigger);
    second.Button(SDL_GAMEPAD_BUTTON_SOUTH, true);
    pads.Poll(state, true);
    NbaModPickerFeed(state.buttons, state.left_trigger);
    Require(loaded == "test" && guest->Active(), "D-pad + Cross/A loads selected mod and releases guest");

    first.Neutral();
    second.Neutral();
    second.Trigger(0);
    pads.Poll(state, false);
    Require(state.left_trigger >= 127 && state.left_trigger <= 128, "Half trigger normalization");
    NbaModPickerFeed(0, 0);
    second.Button(SDL_GAMEPAD_BUTTON_NORTH, true);
    pads.Poll(state, false);
    NbaModPickerFeed(state.buttons, state.left_trigger);
    Require(!guest->Active(), "Partially pressed L2 opens chooser");
    second.Neutral();
    second.Button(SDL_GAMEPAD_BUTTON_EAST, true);
    pads.Poll(state, true);
    NbaModPickerFeed(state.buttons, state.left_trigger);
    Require(guest->Active() && loaded == "test", "Circle/B closes without loading");

    second.Neutral();
    first.Button(SDL_GAMEPAD_BUTTON_SOUTH, true);
    Require(pads.Poll(state, false) && state.id == first.id && state.buttons == 0x1000,
            "Recovery dialog can accept A from another controller");
    first.Neutral();
    second.Button(SDL_GAMEPAD_BUTTON_NORTH, true);
    second.Trigger(SDL_JOYSTICK_AXIS_MAX);
    Require(pads.Poll(state, false) && state.id == second.id, "Return ownership to second pad");

    second.Disconnect();
    Require(!pads.Poll(state, true) && state.id == 0 && state.buttons == 0 &&
            state.left_trigger == 0, "Owner disconnect must not switch to another pad");
    Require(pads.Poll(state, false) && state.id == first.id, "Remaining pad recovered");
    first.Disconnect();
    Require(!pads.Poll(state, false), "All disconnected");
    VirtualPad reconnected("Reconnected virtual pad");
    Require(pads.Poll(state, false) && state.id == reconnected.id, "Hotplug without restart");

    main_menu = false;
    Require(!NbaModPickerFeed(0x8000, 255) && guest->Active(), "Chooser cannot open during gameplay");
    NbaModPickerUseInput(nullptr);
    std::cout << "SDL mod chooser: discovery, L2/Triangle, ownership, button mapping, trigger ranges,\n"
                 "navigation/load/close, guest gating, disconnect and reconnect passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
