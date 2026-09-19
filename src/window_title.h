// NBA JAM: On Fire Edition - what the window calls itself
//
// See window_title.cpp.

#pragma once

#include <string>

namespace rex {
namespace ui {
class ImGuiDrawer;
class Window;
}  // namespace ui
}  // namespace rex

// The name on the window, without the frame rate.
const std::string& NbaWindowTitle();

// One guest frame, counted where the game reads the pad.
void NbaGuestFrame();

// Start keeping the frame rate in the title. Safe to call once the window and
// the ImGui drawer both exist.
void NbaTrackFrameRateInTitle(rex::ui::ImGuiDrawer* drawer,
                              rex::ui::Window* window);
