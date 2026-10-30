// NBA JAM: On Fire Edition - the video settings a player can reach in-game
//
// See video_settings.cpp.

#pragma once

#include <functional>
#include <string>
#include <vector>

// One line of the Video tab: what it is called, what it can be, and which
// of those it is now. `restart` is set on the lines that only take effect at
// the next launch.
struct NbaVideoRow {
  std::string name;
  std::vector<std::string> choices;
  int at = 0;
  bool restart = false;
};

// The tab's lines as they stand.
std::vector<NbaVideoRow> NbaVideoRows();

// Set one line to one of its choices, apply it if it can be applied while the
// game runs, and write it to the settings file.
void NbaVideoSet(int row, int at);

// Every line back to how a fresh install starts.
void NbaVideoReset();

// How to run something on the UI thread, where anything that touches the
// window has to happen. Until it is given, changes are made where they are
// asked for.
void NbaVideoUseUiThread(std::function<void(std::function<void()>)> run);

// Whether anything chosen since launch is waiting for a restart.
bool NbaVideoRestartPending();

// Whether this is a Windows build running under Wine or Proton - which is
// what a Steam Deck is.
bool NbaRunningUnderWine();
