// NBA JAM: On Fire Edition - noticing when the game has stopped, on Linux
//
// The Windows watchdog (watchdog.cpp) suspends the game's threads to read
// where each one is stuck, names the C++ exception that took a thread down,
// and offers a restart over the frozen picture. All of that is built on
// Windows' thread and exception APIs. Until it has a Linux equivalent, a
// Linux build does without: a game that stops just stops, and its log says
// how far it got.

#include "watchdog.h"

#include <rex/logging.h>

void NbaStartWatchdog() {
  REXLOG_INFO("watchdog: not on this system yet - a hang will not be reported");
}

void NbaWatchdogWatchWindow(void*) {}

void NbaWatchdogCreate(rex::ui::ImGuiDrawer*) {}
