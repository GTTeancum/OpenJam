// NBA JAM: On Fire Edition - switching mods from inside the game
//
// The main menu lists the mods that are installed and marks the active one.
// Holding the left trigger and pressing Y opens that list to be chosen from -
// src/mod_picker.cpp draws it - and choosing one relaunches the process
// against that mod's game root, coming back up through the title sequence,
// which is where the animated logo comes from.
//
// There is no faster way to do it. Archives, databases, textures and the
// whole front end are read once at boot and cached; swapping them underneath
// a running game would mean tearing down and rebuilding nearly everything the
// runtime holds.
//
// Why the left trigger as well as Y. A swap is a restart, so it must not be
// possible to set one off by accident in the middle of a match, and Y is a
// real button there - the game's own How To Play says to press Turbo and Y to
// throw elbows. The obvious guard is to only accept Y while the main menu is
// on screen, and the runtime cannot tell: the one hook that sees front-end
// script running, src/apt_trace.cpp, is only ever handed the framework's
// per-frame block, because a screen's own handlers are called inside the
// interpreter without coming back through the entry the hook sits on.
//
// So it takes a second input, and it cannot be Back - that is JAM Central on
// this screen. The left trigger is free, and being a trigger it is not one of
// the buttons: it arrives as a byte further into the gamepad, which is why
// the picker is handed one separately.
//
// **Which mods there are.** `python tools/dlc.py stage` builds the game
// folder, with one root under mods\ per mod plus the base game beside the
// executable, and writes the same mods.list into each. That file is the only
// thing this reads:
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

#include <rex/logging.h>

namespace {

// The variable the replacement is told to wait on, and how long it is given.
// Generous because it only ever costs anything when something has gone wrong:
// normally the process being replaced is gone in a few milliseconds.
const wchar_t* const kFromPid = L"NBAJAM_SWAP_FROM_PID";
constexpr DWORD kWaitForExitMs = 10000;

std::mutex g_lock;
bool g_swapping = false;
bool g_relaunched = false;
std::string g_root;

std::string Narrow(const std::wstring& w) {
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()),
                                    nullptr, 0, nullptr, nullptr);
  std::string out(size_t(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), out.data(), n,
                      nullptr, nullptr);
  return out;
}

std::string CommandLineRoot() {
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
    out = Narrow(v);
  }
  LocalFree(argv);
  return out;
}

// Whichever root is actually being played. The resolved one first: a staged
// game has no command line to read, and when there is one the two agree.
std::string GameDataRoot() {
  return g_root.empty() ? CommandLineRoot() : g_root;
}

std::string Trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
  return s.substr(a, b - a);
}

bool ReadList(const std::string& root, std::vector<NbaMod>* mods,
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
    // id, root, name, and then what the chooser says about it. A line that
    // stops early - one written before those were carried - still reads.
    std::string field[6];
    size_t at = 0;
    for (int i = 0; i < 6 && at <= rest.size(); ++i) {
      const size_t tab = rest.find('\t', at);
      field[i] = Trim(rest.substr(
          at, tab == std::string::npos ? std::string::npos : tab - at));
      at = (tab == std::string::npos) ? rest.size() + 1 : tab + 1;
    }
    NbaMod m{field[0], field[1], field[2].empty() ? field[0] : field[2],
             field[3], field[4], field[5]};
    if (!m.id.empty() && !m.path.empty()) mods->push_back(m);
  }
  return !mods->empty();
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
// size, a GPU plugin, a log level - is what they get back. A staged game is
// launched with nothing at all, so then the root is simply added.
std::wstring RelaunchCommand(const NbaMod& next) {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  const std::wstring root = L"--game_data_root=\"" + Widen(next.path) + L"\"";
  std::wstring out;
  bool said = false;
  for (int i = 0; i < argc; ++i) {
    std::wstring a(argv[i]);
    if (a.rfind(L"--game_data_root=", 0) == 0) {
      a = root;
      said = true;
    } else if (a.find(L' ') != std::wstring::npos && a.front() != L'"' &&
               a.rfind(L"--", 0) != 0) {
      a = L"\"" + a + L"\"";
    }
    if (i) out += L" ";
    out += a;
  }
  LocalFree(argv);
  if (!said) {
    out += L" " + root;
  }
  return out;
}

}  // namespace

bool NbaModList(std::vector<NbaMod>* mods, size_t* active) {
  const std::string root = GameDataRoot();
  std::string id;
  mods->clear();
  *active = 0;
  if (root.empty() || !ReadList(root, mods, &id)) {
    REXLOG_WARN("mods: no mods.list in '{}' - run tools/dlc.py stage", root);
    return false;
  }
  for (size_t i = 0; i < mods->size(); ++i) {
    if ((*mods)[i].id == id) *active = i;
  }
  return true;
}

void NbaModSwapTo(const NbaMod& next) {
  if (next.path.empty() || next.path == GameDataRoot()) {
    REXLOG_INFO("mod swap: '{}' is the one already running", next.name);
    return;
  }
  {
    std::lock_guard<std::mutex> guard(g_lock);
    if (g_swapping) return;
    g_swapping = true;
  }
  REXLOG_INFO("mod swap: -> {} ({})", next.id, next.path);

  wchar_t exe[MAX_PATH]{};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring cmd = RelaunchCommand(next);
  std::vector<wchar_t> buf(cmd.begin(), cmd.end());
  buf.push_back(L'\0');

  // Tell the replacement who to wait for. Both processes exist for a moment,
  // and the guest's memory has one address it wants; the second one to ask
  // for it is given somewhere else and every hook in the port then reads the
  // wrong place. Inherited through the environment, so it costs no flag.
  const std::wstring me = std::to_wstring(GetCurrentProcessId());
  SetEnvironmentVariableW(kFromPid, me.c_str());

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  // Start it in the root it is going to play, the folder default.xex sits in,
  // so the replacement runs from its own game folder exactly as a fresh
  // launch does - and writes its logs and its saves there rather than in
  // whichever root it came from.
  const std::wstring dir = Widen(next.path);
  if (!CreateProcessW(exe, buf.data(), nullptr, nullptr, FALSE, 0, nullptr,
                      dir.c_str(), &si, &pi)) {
    REXLOG_ERROR("mod swap: could not relaunch ({})", GetLastError());
    std::lock_guard<std::mutex> guard(g_lock);
    g_swapping = false;
    return;
  }
  // Hand the right to come to the front over to the replacement. Windows
  // will not let a process raise a window on its own account, but it will let
  // the one currently in front nominate its successor - which is this, the
  // frame before it goes.
  AllowSetForegroundWindow(pi.dwProcessId);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  // Leave at once. The replacement is already starting, and everything this
  // process holds open - the archives above all - has to be let go before it
  // gets there.
  TerminateProcess(GetCurrentProcess(), 0);
}

bool NbaModSwapIsRelaunch() { return g_relaunched; }

void NbaModSwapTakeForeground(void* hwnd) {
  if (!g_relaunched || !hwnd) return;
  HWND window = static_cast<HWND>(hwnd);
  // The game it replaced gave up the right to the foreground on its way out,
  // so this is taking back what the person was already looking at rather than
  // grabbing the screen from whatever else they are doing.
  ShowWindow(window, SW_SHOW);
  BringWindowToTop(window);
  SetForegroundWindow(window);
  SetActiveWindow(window);
  if (GetForegroundWindow() == window) return;

  // The polite request is only honoured while the right handed over by the
  // outgoing process still stands, and a reload takes fifteen seconds -
  // long enough for something else to have come to the front and taken it
  // away. Asking again while attached to whatever is in front now counts as
  // the same thread asking, which is allowed.
  const HWND front = GetForegroundWindow();
  if (!front) return;
  const DWORD theirs = GetWindowThreadProcessId(front, nullptr);
  const DWORD ours = GetCurrentThreadId();
  if (theirs == ours) return;
  if (!AttachThreadInput(ours, theirs, TRUE)) return;
  BringWindowToTop(window);
  SetForegroundWindow(window);
  SetActiveWindow(window);
  AttachThreadInput(ours, theirs, FALSE);
}

void NbaModSwapUseRoot(const std::filesystem::path& root) {
  g_root = root.empty() ? std::string() : Narrow(root.wstring());
}

void NbaModSwapWaitForPredecessor() {
  wchar_t buf[32]{};
  const DWORD n = GetEnvironmentVariableW(kFromPid, buf, 32);
  if (n == 0 || n >= 32) return;
  // Once only: a swap from here should not wait again, and nothing else
  // launched from this process should inherit it either.
  SetEnvironmentVariableW(kFromPid, nullptr);
  const DWORD pid = DWORD(wcstoul(buf, nullptr, 10));
  if (pid == 0) return;
  g_relaunched = true;
  HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
  if (!h) return;      // already gone, which is the common case
  WaitForSingleObject(h, kWaitForExitMs);
  CloseHandle(h);
}
