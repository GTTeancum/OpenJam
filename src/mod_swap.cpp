// NBA JAM: On Fire Edition - switching mods from inside the game
//
// The main menu lists the mods that are installed and marks the active one.
// BACK and Y together moves to the next one in the list: the process
// relaunches against that mod's game root and comes back up through the title
// sequence, which is where the animated logo comes from. There is no faster
// way to do it. Archives, databases, textures and the whole front end are read
// once at boot and cached; swapping them underneath a running game would mean
// tearing down and rebuilding nearly everything the runtime holds.
//
// Why two buttons rather than the Y the footer offers. A swap is a restart, so
// it must not be possible to trigger one by accident in the middle of a match,
// and Y is a real button there. The obvious guard is to only accept Y while
// the main menu is on screen - but the runtime cannot tell. The one hook that
// sees front-end script running, src/apt_trace.cpp, only ever gets handed the
// framework's per-frame block: five distinct blocks run on the main menu and
// all of them belong to bounce/framework.ast, because a screen's own handlers
// are called inside the interpreter without coming back through the entry the
// hook sits on. So there is no cheap way to know which screen is up, and the
// trigger is instead one nothing else uses.
//
// **Which mods there are.** `python tools/dlc.py roots` builds one game root
// per mod, plus one for the base game, and writes the same mods.list into
// each of them. That file is the only thing this reads:
//
//     active <id>
//     mod <id> <TAB> <path> <TAB> <name>
//
// Tab separated because a mod's name has spaces in it and its path might too.
//
// Saves are not in here and do not need to be. Each root says which mod it
// holds and src/mod_saves.cpp turns that into a save folder, so a relaunch
// carries nothing about saves and still lands on the right ones.

#include "mod_swap.h"

#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include <rex/logging.h>

namespace {

constexpr uint16_t kButtonBack = 0x0020;
constexpr uint16_t kButtonY = 0x8000;

struct Root {
  std::string id;
  std::string path;
  std::string name;
};

std::mutex g_lock;
bool g_swapping = false;

std::string GameDataRoot() {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::string out;
  for (int i = 1; i < argc && out.empty(); ++i) {
    std::wstring a(argv[i]);
    const std::wstring key = L"--game_data_root=";
    if (a.rfind(key, 0) != 0) continue;
    std::wstring v = a.substr(key.size());
    if (v.size() >= 2 && v.front() == L'"' && v.back() == L'"') {
      v = v.substr(1, v.size() - 2);
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, v.c_str(), int(v.size()),
                                      nullptr, 0, nullptr, nullptr);
    out.assign(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, v.c_str(), int(v.size()), out.data(), n,
                        nullptr, nullptr);
  }
  LocalFree(argv);
  return out;
}

std::string Trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
  return s.substr(a, b - a);
}

bool ReadList(const std::string& root, std::vector<Root>* roots,
              std::string* active) {
  std::ifstream in(root + "\\mods.list");
  if (!in) return false;
  std::string line;
  while (std::getline(in, line)) {
    line = Trim(line);
    if (line.empty() || line[0] == '#') continue;
    if (line.rfind("active", 0) == 0) {
      *active = Trim(line.substr(6));
      continue;
    }
    if (line.rfind("mod", 0) != 0) continue;
    const std::string rest = Trim(line.substr(3));
    std::string field[3];
    size_t at = 0;
    for (int i = 0; i < 3 && at <= rest.size(); ++i) {
      const size_t tab = rest.find('\t', at);
      field[i] = Trim(rest.substr(
          at, tab == std::string::npos ? std::string::npos : tab - at));
      at = (tab == std::string::npos) ? rest.size() + 1 : tab + 1;
    }
    Root r{field[0], field[1], field[2].empty() ? field[0] : field[2]};
    if (!r.id.empty() && !r.path.empty()) roots->push_back(r);
  }
  return !roots->empty();
}

std::wstring Widen(const std::string& s) {
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()),
                                    nullptr, 0);
  std::wstring out(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), out.data(), n);
  return out;
}

// The same command line, with the game root pointed somewhere else. Every
// other flag is passed through: whatever the person launched with - a window
// size, a GPU plugin, a log level - is what they get back.
std::wstring RelaunchCommand(const Root& next) {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::wstring out;
  for (int i = 0; i < argc; ++i) {
    std::wstring a(argv[i]);
    if (a.rfind(L"--game_data_root=", 0) == 0) {
      a = L"--game_data_root=\"" + Widen(next.path) + L"\"";
    } else if (a.find(L' ') != std::wstring::npos && a.front() != L'"' &&
               a.rfind(L"--", 0) != 0) {
      a = L"\"" + a + L"\"";
    }
    if (i) out += L" ";
    out += a;
  }
  LocalFree(argv);
  return out;
}

void Swap() {
  const std::string root = GameDataRoot();
  std::vector<Root> roots;
  std::string active;
  if (root.empty() || !ReadList(root, &roots, &active)) {
    REXLOG_WARN("mod swap: no mods.list in '{}' - run tools/dlc.py roots", root);
    return;
  }
  size_t at = 0;
  for (size_t i = 0; i < roots.size(); ++i) {
    if (roots[i].id == active) at = i;
  }
  const Root& next = roots[(at + 1) % roots.size()];
  if (next.path == root) {
    REXLOG_INFO("mod swap: '{}' is the only root installed", next.name);
    return;
  }
  REXLOG_INFO("mod swap: {} -> {}", active, next.id);

  wchar_t exe[MAX_PATH]{};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring cmd = RelaunchCommand(next);
  std::vector<wchar_t> buf(cmd.begin(), cmd.end());
  buf.push_back(L'\0');

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  wchar_t dir[MAX_PATH]{};
  GetCurrentDirectoryW(MAX_PATH, dir);
  if (!CreateProcessW(exe, buf.data(), nullptr, nullptr, FALSE, 0, nullptr,
                      dir, &si, &pi)) {
    REXLOG_ERROR("mod swap: could not relaunch ({})", GetLastError());
    return;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  // Leave at once. The replacement is already starting, and everything this
  // process holds open - the archives above all - has to be let go before it
  // gets there.
  TerminateProcess(GetCurrentProcess(), 0);
}

}  // namespace

void NbaModSwapPad(uint16_t buttons) {
  static uint16_t last = 0;
  const uint16_t both = uint16_t(kButtonBack | kButtonY);
  const bool now_held = (buttons & both) == both;
  const bool was_held = (last & both) == both;
  last = buttons;
  if (!now_held || was_held) return;      // only on the frame it completes

  {
    std::lock_guard<std::mutex> guard(g_lock);
    if (g_swapping) return;
    g_swapping = true;
  }
  Swap();
}
