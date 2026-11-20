// NBA JAM: On Fire Edition - the video settings, as the game's Settings screen
// shows them
//
// The runtime has the settings; what it lacks is a way for a player to reach
// them that is not a text file or the F4 developer editor. This is the model
// behind the Video tab src/video_tab.cpp adds to the game's own Settings
// screen: each line is a short list of choices, written to
// nbajam_ofe.toml the moment it changes so the next launch starts the way
// this one was left.
//
// Two kinds of setting. Some the runtime reads every frame, and those are
// applied on the spot: windowed or fullscreen, the picture's shape, and
// vsync. Output resolution, render scale and refresh rate are read at
// startup. The tab remembers those choices for the next launch without
// changing the running game's resolution, render targets or refresh rate.
//
// Only these keys are touched in the file. Everything else in it - a
// hand-written line, a key the F4 editor saved - stays as it was.

#include "video_settings.h"

#include "host.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/window_sdl.h>
#include <toml++/toml.hpp>

REXCVAR_DEFINE_BOOL(match_display_refresh, false, "Display",
                    "Match the game's refresh rate to the display at startup")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace {

constexpr auto kMatchDisplayRefresh = "match_display_refresh";
constexpr auto kGuestRefreshRate = "video_mode_refresh_rate";

struct Size {
  int w, h;
};
// 1280x800 is a Steam Deck's screen. The game draws itself at whatever shape
// it is given, so that one fills the Deck with no bars and nothing stretched.
constexpr Size kSizes[] = {{1280, 720}, {1280, 800}, {1600, 900},
                           {1920, 1080}, {2560, 1440}, {3840, 2160}};
constexpr int kScales[] = {1, 2, 3};

std::filesystem::path SettingsFile() {
  const auto dir = NbaExeDir();
  return dir.empty() ? dir : dir / "nbajam_ofe.toml";
}

constexpr std::string_view kUtf8Bom = "\xef\xbb\xbf";

// TOML source positions count Unicode code points, not bytes. The parser
// already validated UTF-8; skip continuation bytes when advancing a column.
std::optional<size_t> ByteOffset(std::string_view text, toml::source_position pos) {
  if (!pos) return std::nullopt;
  size_t offset = text.starts_with(kUtf8Bom) ? kUtf8Bom.size() : 0;
  for (toml::source_index line = 1; line < pos.line; ++line) {
    offset = text.find('\n', offset);
    if (offset == std::string_view::npos) return std::nullopt;
    ++offset;
  }
  for (toml::source_index column = 1; column < pos.column; ++column) {
    if (offset == text.size() || text[offset] == '\n') return std::nullopt;
    ++offset;
    while (offset < text.size() && (static_cast<unsigned char>(text[offset]) & 0xc0) == 0x80) {
      ++offset;
    }
  }
  return offset;
}

// Replace only the values of existing top-level keys, using parsed positions
// so comments, nested tables and multiline values for other keys stay intact.
// Put new keys before any table headers; they are not members of a table.
bool WriteKeys(const std::vector<std::pair<std::string, std::string>>& keys) {
  const std::filesystem::path file = SettingsFile();
  if (file.empty()) return false;

  std::string text;
  {
    std::ifstream in(file, std::ios::binary);
    if (in) {
      char block[4096];
      do {
        in.read(block, sizeof(block));
        text.append(block, static_cast<size_t>(in.gcount()));
      } while (in);
      if (in.bad() || !in.eof()) {
        REXLOG_WARN("video: could not read {}", file.string());
        return false;
      }
    } else {
      std::error_code ec;
      if (std::filesystem::exists(file, ec) || ec) {
        REXLOG_WARN("video: could not read {}", file.string());
        return false;
      }
    }
  }

  struct Edit {
    size_t begin, end;
    std::string_view value;
  };
  std::vector<Edit> edits;
  std::string prefix;
  const auto first_newline = text.find('\n');
  const bool crlf = first_newline != std::string::npos && first_newline > 0 &&
                    text[first_newline - 1] == '\r';
  const char* newline = crlf ? "\r\n" : "\n";
  try {
    const auto config = toml::parse(text);
    for (const auto& [key, value] : keys) {
      const auto* old = config.get(key);
      if (!old) {
        prefix += key + " = " + value + newline;
        continue;
      }
      const auto start = ByteOffset(text, old->source().begin);
      const auto end = ByteOffset(text, old->source().end);
      if (!old->is_value() || !start || !end || *start >= *end) {
        REXLOG_WARN("video: cannot replace the setting '{}' in {}", key, file.string());
        return false;
      }
      edits.push_back({*start, *end, value});
    }
  } catch (const toml::parse_error&) {
    REXLOG_WARN("video: not modifying malformed settings in {}", file.string());
    return false;
  }

  // Replace from the end so later edits don't move earlier source offsets.
  std::sort(edits.begin(), edits.end(), [](const Edit& a, const Edit& b) {
    return a.begin > b.begin;
  });
  for (const auto& edit : edits) {
    text.replace(edit.begin, edit.end - edit.begin, edit.value);
  }
  if (!prefix.empty()) {
    const size_t bom = text.starts_with(kUtf8Bom) ? kUtf8Bom.size() : 0;
    size_t insert_at = bom;
    // Keep leading comments, including the SDK header used by config_repair.
    while (insert_at < text.size()) {
      const size_t end = text.find('\n', insert_at);
      const size_t line_end = end == std::string::npos ? text.size() : end;
      const auto line = std::string_view(text).substr(insert_at, line_end - insert_at);
      const size_t content = line.find_first_not_of(" \t\r");
      if (content != std::string_view::npos && line[content] != '#') break;
      if (end == std::string::npos) {
        text += newline;
        insert_at = text.size();
        break;
      }
      insert_at = end + 1;
    }
    text.insert(insert_at, prefix);
  }

  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  out << text;
  out.close();
  if (!out) {
    REXLOG_WARN("video: could not write {}", file.string());
    return false;
  }
  return true;
}

// What the restart-only lines were when the game started, and what has been
// chosen for next time. Taken the first time the tab asks.
std::mutex g_lock;
bool g_taken = false;
int g_size_running = 0, g_size_next = 0;
int g_scale_running = 0, g_scale_next = 0;
bool g_refresh_running = false;

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

enum Row { kDisplay, kWindow, kSharpness, kShape, kVsync, kRefreshRate, kRows };

const char* Bool(bool b) { return b ? "true" : "false"; }

}  // namespace

std::vector<NbaVideoRow> NbaVideoRows() {
  std::lock_guard<std::mutex> guard(g_lock);
  Take();
  std::vector<NbaVideoRow> rows(kRows);

  rows[kDisplay].name = "DISPLAY";
  rows[kDisplay].choices = {"WINDOW", "FULLSCREEN"};
  rows[kDisplay].at = rex::cvar::Query<bool>("fullscreen") ? 1 : 0;

  rows[kWindow].name = "OUTPUT RESOLUTION";
  for (const Size& s : kSizes) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d X %d", s.w, s.h);
    rows[kWindow].choices.push_back(buf);
  }
  rows[kWindow].at = g_size_next;
  rows[kWindow].restart = true;

  rows[kSharpness].name = "RENDER SCALE";
  rows[kSharpness].choices = {"ORIGINAL (1X)", "HIGH (2X)", "ULTRA (3X)"};
  rows[kSharpness].at = g_scale_next;
  rows[kSharpness].restart = true;

  rows[kShape].name = "PICTURE";
  rows[kShape].choices = {"KEEP SHAPE", "STRETCH"};
  rows[kShape].at = rex::cvar::Query<bool>("present_letterbox") ? 0 : 1;

  rows[kVsync].name = "VSYNC";
  rows[kVsync].choices = {"ON", "OFF"};
  rows[kVsync].at = rex::cvar::Query<bool>("vsync") ? 0 : 1;

  rows[kRefreshRate].name = "REFRESH RATE";
  rows[kRefreshRate].choices = {"ORIGINAL", "MATCH DISPLAY"};
  rows[kRefreshRate].at = rex::cvar::Query<bool>(kMatchDisplayRefresh) ? 1 : 0;
  rows[kRefreshRate].restart = true;

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
      const std::string scale = std::to_string(kScales[g_scale_next]);
      // Explicit per-axis settings take precedence in the SDK. Update all
      // three so selecting a scale also replaces any earlier axis overrides.
      WriteKeys({{"resolution_scale", scale},
                 {"draw_resolution_scale_x", scale},
                 {"draw_resolution_scale_y", scale}});
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
    case kRefreshRate: {
      const char* match = Bool(at == 1);
      if (WriteKeys({{kMatchDisplayRefresh, match}})) {
        // This preference is only used at startup. Also updating the CVar
        // keeps the F4 editor from saving the previously selected value.
        rex::cvar::SetFlagByName(kMatchDisplayRefresh, match);
      }
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
  Set(kRefreshRate, 0);
}

void NbaVideoUseUiThread(std::function<void(std::function<void()>)> run) {
  std::lock_guard<std::mutex> guard(g_lock);
  g_ui = std::move(run);
}

bool NbaVideoRestartPending() {
  std::lock_guard<std::mutex> guard(g_lock);
  Take();
  return g_size_next != g_size_running || g_scale_next != g_scale_running ||
         rex::cvar::Query<bool>(kMatchDisplayRefresh) != g_refresh_running;
}

void NbaConfigureDisplayRefresh(const rex::ui::Window* window) {
  g_refresh_running = rex::cvar::Query<bool>(kMatchDisplayRefresh);
  if (!g_refresh_running) return;

  const auto* rate = rex::cvar::GetFlagInfo(kGuestRefreshRate);
  if (!rate) return;
  if (rate->source != rex::cvar::Source::kDefault) {
    REXLOG_INFO("video: keeping the explicitly configured guest refresh rate");
    return;
  }

  // Window::Create on this SDK always makes a WindowSDL. Query inside the
  // runtime that owns it, not the executable's potentially separate SDL.
  const double refresh = window
      ? static_cast<const rex::ui::WindowSDL*>(window)->GetCurrentDisplayRefreshRate()
      : 0.0;
  if (!std::isfinite(refresh) || refresh < rate->constraints.min.value_or(24.0) ||
      refresh > rate->constraints.max.value_or(240.0)) {
    REXLOG_WARN("video: current display refresh rate unavailable or unsupported; keeping Original");
    return;
  }

  // The SDK uses this one rate for the mode reported to the guest and the
  // emulated VBlank timer. Both must agree for the game's swap timing.
  auto* entry = const_cast<rex::cvar::FlagEntry*>(rate);
  if (entry->setter(std::to_string(refresh))) {
    // Like the controller mapping path, this is a launch-local default.
    // F4 must save the preference, not this display's measured value.
    entry->default_value = entry->getter();
    REXLOG_INFO("video: matching current display at {:.3f} Hz", refresh);
  }
}

bool NbaRunningUnderWine() {
#ifdef _WIN32
  static const bool wine = [] {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll && GetProcAddress(ntdll, "wine_get_version") != nullptr;
  }();
  return wine;
#else
  return false;   // a native build is not running under Wine by definition
#endif
}

bool NbaRunningOnSteamDeck() {
  static const bool deck = NbaEnv("SteamDeck") == "1";
  return deck;
}
