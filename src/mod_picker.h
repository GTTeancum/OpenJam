// NBA JAM: On Fire Edition - the list of mods, on screen, to choose from
//
// See mod_picker.cpp.

#pragma once

#include <cstdint>

struct ImFontAtlas;
struct ImFont;

namespace rex {
namespace input {
class InputSystem;
}  // namespace input
}  // namespace rex

namespace rex {
namespace ui {
class ImGuiDrawer;
}  // namespace ui
}  // namespace rex

// Add the game's own typefaces, so the list is drawn in them rather than in
// the runtime's debug font. Call from OnConfigureFonts; missing files are not
// an error, the list just falls back to the default face.
void NbaModPickerFonts(ImFontAtlas* atlas);

// The game's two typefaces, once NbaModPickerFonts has loaded them: the one
// the front end sets headings in, and the one it sets everything else in.
// Both fall back to the runtime's own face when the game's are not there,
// so they are never null once ImGui is up.
ImFont* NbaDisplayFont();
ImFont* NbaTextFont();

// The runtime's input system: where the chooser reads the pad's effect on the
// game from, so that the menu underneath holds still while the list is being
// walked. Safe to call once the runtime is up.
void NbaModPickerUseInput(rex::input::InputSystem* input);

// Start drawing. Safe to call once the ImGui drawer exists.
void NbaModPickerCreate(rex::ui::ImGuiDrawer* drawer);

// The pad, as the kernel filled it in, before the game has seen it. The left
// trigger comes separately because it is a byte in the gamepad rather than
// one of the buttons.
//
// Returns true while the picker has the pad, which means the caller should
// hand the game an empty one: otherwise moving down the mod list also moves
// down the menu behind it.
bool NbaModPickerPad(uint16_t buttons, uint8_t left_trigger);

// The same, from whichever pad is being listened to. Not for the guest hook:
// that one goes through NbaModPickerPad, which stands aside when the
// controller is being read here instead.
bool NbaModPickerFeed(uint16_t buttons, uint8_t left_trigger);
