// NBA JAM: On Fire Edition - the video settings, as the game's Settings screen
// shows them
//
// The runtime has the settings; what it lacks is a way for a player to reach
// them that is not a text file or the F4 developer editor. This is the model
// behind the Video tab src/video_tab.cpp adds to the game's own Settings
// screen: five lines, each a short list of choices, each written to
// nbajam_ofe.toml the moment it changes so the next launch starts the way
// this one was left.
//
// Two kinds of setting. Some the runtime reads every frame, and those are
// applied on the spot: windowed or fullscreen, the picture's shape, and
// vsync. The others are read once while the runtime sets itself up - the
// resolution, which the game also takes as the video mode it renders for, and
// the sharpness, which sizes the GPU's render targets. Changing those under a
// running game is not a smaller version of the effect; it is a different and
// worse one. So they are only written to the file, the tab says they apply
// the next time the game starts, and the running game is left alone.
//
// Only these keys are touched in the file. Everything else in it - a
// hand-written line, a key the F4 editor saved - stays as it was.

#include "video_settings.h"

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <functional>
#include <fstream>
#include <mutex>
#include <sstream>

#include <rex/cvar.h>
#include <rex/logging.h>

namespace {

struct Size {
  int w, h;
};
// 1280x800 is a Steam Deck's screen. The game draws itself at whatever shape
// it is given, so that one fills the Deck with no bars and nothing stretched.
constexpr Size kSizes[] = {{1280, 720}, {1280, 800}, {1600, 900},
                           {1920, 1080}, {2560, 1440}};
constexpr int kScales[] = {1, 2, 3};

std::filesystem::path SettingsFile() {
  wchar_t buf[MAX_PATH]{};
  if (!GetModuleFileNameW(nullptr, buf, MAX_PATH)) return {};
  return std::filesystem::path(buf).parent_path() / "nbajam_ofe.toml";
}

// Rewrite `key = value` lines for the given keys, adding any that are not
// there, and leave every other line alone.
void WriteKeys(const std::vector<std::pair<std::string, std::string>>& keys) {
  const std::filesystem::path file = SettingsFile();
  if (file.empty()) return;
  std::vector<std::string> lines;
  {
    std::ifstream in(file, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      lines.push_back(line);
    }
  }
  for (const auto& [key, value] : keys) {
    bool found = false;
    for (std::string& line : lines) {
      const auto start = line.find_first_not_of(" \t");
      if (start == std::string::npos || line.compare(start, key.size(), key) != 0)
        continue;
      const auto after = line.find_first_not_of(" \t", start + key.size());
      if (after == std::string::npos || line[after] != '=') continue;
      line = key + " = " + value;
      found = true;
    }
    if (!found) lines.push_back(key + " = " + value);
  }
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  if (!out) {
    REXLOG_WARN("video: could not write {}", file.string());
    return;
  }
  for (const std::string& line : lines) out << line << '\n';
}

// What the restart-only lines were when the game started, and what has been
// chosen for next time. Taken the first time the tab asks.
std::mutex g_lock;
bool g_taken = false;
int g_size_running = 0, g_size_next = 0;
int g_scale_running = 0, g_scale_next = 0;

int SizeIndex(int w, int h) {
  if (w <= 0 || h <= 0) return 0;  // the runtime's default is 1280x720
  for (int i = 0; i < int(std::size(kSizes)); ++i) {
    if (kSizes[i].w == w && kSizes[i].h == h) return i;
  }
  return 0;
}

int ScaleIndex(int s) {
  for (int i = 0; i < int(std::size(kScales)); ++i) {
    if (kScales[i] == s) return i;
  }
  return 0;
}

void Take() {
  if (g_taken) return;
  g_taken = true;
  g_size_running = g_size_next =
      SizeIndex(rex::cvar::Query<int32_t>("window_width"),
                rex::cvar::Query<int32_t>("window_height"));
  g_scale_running = g_scale_next =
      ScaleIndex(rex::cvar::Query<int32_t>("resolution_scale"));
}

enum Row { kDisplay, kWindow, kSharpness, kShape, kVsync, kRows };

const char* Bool(bool b) { return b ? "true" : "false"; }

}  // namespace

std::vector<NbaVideoRow> NbaVideoRows() {
  std::lock_guard<std::mutex> guard(g_lock);
  Take();
  std::vector<NbaVideoRow> rows(kRows);

  rows[kDisplay].name = "DISPLAY";
  rows[kDisplay].choices = {"WINDOW", "FULLSCREEN"};
  rows[kDisplay].at = rex::cvar::Query<bool>("fullscreen") ? 1 : 0;

  rows[kWindow].name = "RESOLUTION";
  for (const Size& s : kSizes) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d X %d", s.w, s.h);
    rows[kWindow].choices.push_back(buf);
  }
  rows[kWindow].at = g_size_next;
  rows[kWindow].restart = true;

  rows[kSharpness].name = "SHARPNESS";
  rows[kSharpness].choices = {"ORIGINAL", "HIGH (2X)", "ULTRA (3X)"};
  rows[kSharpness].at = g_scale_next;
  rows[kSharpness].restart = true;

  rows[kShape].name = "PICTURE";
  rows[kShape].choices = {"KEEP SHAPE", "STRETCH"};
  rows[kShape].at = rex::cvar::Query<bool>("present_letterbox") ? 0 : 1;

  rows[kVsync].name = "VSYNC";
  rows[kVsync].choices = {"ON", "OFF"};
  rows[kVsync].at = rex::cvar::Query<bool>("vsync") ? 0 : 1;
  return rows;
}

namespace {

// Where changes that touch the window are made. The game asks for a change
// from its own thread, and switching to fullscreen from there takes the
// window down with it: the runtime resizes the window from inside the
// setting's change notice, which has to happen on the thread that owns it.
std::function<void(std::function<void()>)> g_ui;

void Live(const char* key, bool value) {
  auto set = [key = std::string(key), value] {
    rex::cvar::SetFlagByName(key, Bool(value));
  };
  if (g_ui) {
    g_ui(set);
  } else {
    set();
  }
}

// Set one line, apply it if it can be applied now, and write it down. Called
// with the lock held.
void Set(int row, int at) {
  auto clamp = [](int at, int n) { return at < 0 ? 0 : at >= n ? n - 1 : at; };
  switch (row) {
    case kDisplay: {
      const bool on = at == 1;
      Live("fullscreen", on);
      WriteKeys({{"fullscreen", Bool(on)}});
      break;
    }
    case kWindow: {
      g_size_next = clamp(at, int(std::size(kSizes)));
      const Size s = kSizes[g_size_next];
      WriteKeys({{"window_width", std::to_string(s.w)},
                 {"window_height", std::to_string(s.h)}});
      break;
    }
    case kSharpness: {
      g_scale_next = clamp(at, int(std::size(kScales)));
      WriteKeys({{"resolution_scale", std::to_string(kScales[g_scale_next])}});
      break;
    }
    case kShape: {
      const bool keep = at == 0;
      Live("present_letterbox", keep);
      WriteKeys({{"present_letterbox", Bool(keep)}});
      break;
    }
    case kVsync: {
      const bool on = at == 0;
      Live("vsync", on);
      WriteKeys({{"vsync", Bool(on)}});
      break;
    }
    default:
      break;
  }
}

}  // namespace

void NbaVideoSet(int row, int at) {
  std::lock_guard<std::mutex> guard(g_lock);
  Take();
  Set(row, at);
}

void NbaVideoReset() {
  std::lock_guard<std::mutex> guard(g_lock);
  Take();
  Set(kDisplay, NbaRunningUnderWine() ? 1 : 0);
  Set(kWindow, NbaRunningOnSteamDeck() ? 1 : 0);   // 1280x800 on a Deck
  Set(kSharpness, 0);
  Set(kShape, 0);
  Set(kVsync, 0);
}

void NbaVideoUseUiThread(std::function<void(std::function<void()>)> run) {
  std::lock_guard<std::mutex> guard(g_lock);
  g_ui = std::move(run);
}

bool NbaVideoRestartPending() {
  std::lock_guard<std::mutex> guard(g_lock);
  Take();
  return g_size_next != g_size_running || g_scale_next != g_scale_running;
}

bool NbaRunningUnderWine() {
  static const bool wine = [] {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll && GetProcAddress(ntdll, "wine_get_version") != nullptr;
  }();
  return wine;
}

bool NbaRunningOnSteamDeck() {
  static const bool deck = [] {
    char buf[4]{};
    const DWORD n = GetEnvironmentVariableA("SteamDeck", buf, sizeof(buf));
    return n > 0 && n < sizeof(buf) && buf[0] == '1';
  }();
  return deck;
}
