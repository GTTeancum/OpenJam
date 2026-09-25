// NBA JAM: On Fire Edition - noticing when the game has stopped
//
// See watchdog.cpp.

#pragma once

namespace rex {
namespace ui {
class ImGuiDrawer;
}  // namespace ui
}  // namespace rex

// Start watching. Call once, after the game is up.
void NbaStartWatchdog();

// The game's window. Nothing is offered once it has gone: a player who has
// closed the game is not to be given it back. Safe to call once there is one.
void NbaWatchdogWatchWindow(void* hwnd);

// Draw the way out, for when it is needed. Safe to call once the ImGui
// drawer exists; nothing appears until the game actually stops.
void NbaWatchdogCreate(rex::ui::ImGuiDrawer* drawer);
