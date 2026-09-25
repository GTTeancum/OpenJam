// NBA JAM: On Fire Edition - noticing when the game has stopped
//
// A crash leaves a report behind. A game that simply stops leaves nothing at
// all: the window is still there, still drawing the last thing it drew, and
// the only information a player can give is "it froze on the loading screen".
//
// This watches the guest's own frame counter. When it has not moved for a
// while, and it had been moving before, the game is stuck. Every thread in
// the process is then stopped for as long as it takes to read its call stack
// and started again. Recompiled game code is ordinary C++ functions named
// after the address they came from, so what comes out names the part of the
// game that is going round in circles, and goes in the log next to everything
// else that happened.
//
// It then puts a panel on the screen offering to start the game again. The
// game's own threads are the ones that have stopped; the window, the pad and
// this port's own drawing are all still going, so there is still somebody
// listening even when the game itself is not. Starting again is the same
// thing the mod chooser does - the process is replaced - and it costs the
// fifteen seconds a load takes rather than a trip to Task Manager.
//
// If nothing is pressed within twenty seconds it starts again anyway. The
// thing that has stopped may well have taken the controller with it, and
// waiting for permission that cannot be given leaves a player with a dead
// window and nothing to do about it. Pressing B calls it off and lets the
// game sit there as long as they like.
//
// NBAJAM_STALL_SECONDS sets how long counts as stuck. The default is
// forty-five, and the longest this game has ever gone between frames while
// actually working is under a second, so there is a wide margin before
// anything is said. Setting it to 0 turns the watchdog off entirely.

#include "watchdog.h"

#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#pragma comment(lib, "dbghelp.lib")

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>

#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>

#include "host_pad.h"
#include "mod_picker.h"
#include "mod_swap.h"
#include "window_title.h"

namespace {

constexpr int kDefaultStallSeconds = 45;

// Set when the game has stopped, cleared if it ever starts again or if the
// player would rather wait. Written by the watchdog, read while drawing.
std::atomic<bool> g_stuck{false};

// Set when the game has died rather than merely stopped. The window and this
// port are still going; only the game is gone, so the same way out works.
std::atomic<bool> g_fell_over{false};

// The game's window, once there is one. When it goes, so does any offer to
// start the game again: closing the game is an answer in itself.
std::atomic<void*> g_window{nullptr};

bool StillOpen() {
  HWND w = static_cast<HWND>(g_window.load(std::memory_order_relaxed));
  return !w || IsWindow(w);
}
constexpr int kMaxFrames = 24;       // deep enough to see past the loop
constexpr int kMaxReports = 3;       // then stop; it is the same stack

int StallSeconds() {
  char buf[16]{};
  const DWORD n = GetEnvironmentVariableA("NBAJAM_STALL_SECONDS", buf,
                                          sizeof(buf));
  if (n > 0 && n < sizeof(buf)) {
    const int v = std::atoi(buf);
    if (v >= 0) return v;
  }
  return kDefaultStallSeconds;
}

// The name of whatever is at this address, or an empty string.
std::string NameOf(HANDLE proc, DWORD64 addr) {
  alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 256]{};
  auto* sym = reinterpret_cast<SYMBOL_INFO*>(storage);
  sym->SizeOfStruct = sizeof(SYMBOL_INFO);
  sym->MaxNameLen = 255;
  DWORD64 displacement = 0;
  if (!SymFromAddr(proc, addr, &displacement, sym)) return std::string();
  return std::string(sym->Name);
}

// One thread's stack, innermost first. The thread is stopped while this
// reads it and started again before returning.
std::vector<std::string> StackOf(DWORD tid) {
  std::vector<std::string> out;
  HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME |
                         THREAD_QUERY_INFORMATION, FALSE, tid);
  if (!th) return out;
  if (SuspendThread(th) == DWORD(-1)) {
    CloseHandle(th);
    return out;
  }
  CONTEXT ctx{};
  ctx.ContextFlags = CONTEXT_FULL;
  if (GetThreadContext(th, &ctx)) {
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = ctx.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    const HANDLE proc = GetCurrentProcess();
    for (int i = 0; i < kMaxFrames; ++i) {
      if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, th, &frame, &ctx,
                       nullptr, SymFunctionTableAccess64, SymGetModuleBase64,
                       nullptr)) {
        break;
      }
      if (!frame.AddrPC.Offset) break;
      std::string name = NameOf(proc, frame.AddrPC.Offset);
      if (name.empty()) {
        char hex[32];
        std::snprintf(hex, sizeof(hex), "%016llX",
                      static_cast<unsigned long long>(frame.AddrPC.Offset));
        name = hex;
      }
      out.push_back(name);
    }
  }
  ResumeThread(th);
  CloseHandle(th);
  return out;
}

// Is this a thread running game code, rather than one of the port's own?
bool LooksLikeGuest(const std::vector<std::string>& stack) {
  for (const std::string& name : stack) {
    if (name.compare(0, 4, "sub_") == 0) return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// The way out

const ImU32 kShade = IM_COL32(0, 0, 0, 170);
const ImU32 kPlate = IM_COL32(7, 12, 21, 248);
const ImU32 kEdge = IM_COL32(38, 54, 78, 255);
const ImU32 kOrange = IM_COL32(255, 162, 60, 255);
const ImU32 kInk = IM_COL32(245, 247, 250, 255);
const ImU32 kQuiet = IM_COL32(150, 165, 185, 255);

// How long the panel waits for an answer before starting the game again on
// its own.
constexpr double kGraceSeconds = 20.0;

// Which of these buttons went down since the last look. The pad is read
// here rather than through the game, because the game is the part that has
// stopped.
struct Press {
  bool go = false;      // A, or Enter
  bool wait = false;    // B, or Escape
};

Press JustPressed() {
  static uint16_t was = 0;
  Press p;
  uint16_t now = 0;
  uint8_t trigger = 0;
  if (NbaHostPad(&now, &trigger)) {
    const uint16_t down = uint16_t(now & ~was);
    was = now;
    p.go = (down & 0x1000) != 0;        // XINPUT_GAMEPAD_A
    p.wait = (down & 0x2000) != 0;      // XINPUT_GAMEPAD_B
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
      ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
    p.go = true;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) p.wait = true;
  return p;
}

class WayOut : public rex::ui::ImGuiDialog {
 public:
  explicit WayOut(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (!g_stuck.load(std::memory_order_relaxed)) {
      shown_ = false;
      return;
    }
    if (!shown_) {
      shown_ = true;
      since_ = ImGui::GetTime();
      JustPressed();            // swallow whatever was already held
    }

    const float s = (std::min)(io.DisplaySize.x / 1280.0f,
                               io.DisplaySize.y / 720.0f);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(ImVec2(0, 0), io.DisplaySize, kShade);

    const float w = 620.0f * s;
    const float h = 232.0f * s;
    const ImVec2 tl((io.DisplaySize.x - w) * 0.5f,
                    (io.DisplaySize.y - h) * 0.5f);
    const ImVec2 br(tl.x + w, tl.y + h);
    const float round = 10.0f * s;
    dl->AddRectFilled(tl, br, kPlate, round);
    dl->AddRect(tl, br, kEdge, round, 0, 2.0f * s);

    const float pad = 30.0f * s;
    float y = tl.y + pad;
    const float big = 30.0f * s;
    const float mid = 19.0f * s;

    const bool died = g_fell_over.load(std::memory_order_relaxed);
    dl->AddText(NbaDisplayFont(), big, ImVec2(tl.x + pad, y), kOrange,
                died ? "THE GAME HAS FALLEN OVER" : "THE GAME HAS STOPPED");
    y += big * 1.5f;

    ImFont* f = NbaTextFont();
    const char* stopped[] = {
        "It is not answering any more. This is usually a mod that is",
        "missing a player, a court or a piece of art the game wanted.",
        "",
        "Starting again puts you back at the main menu. Nothing that",
        "was saved is lost.",
    };
    const char* fell[] = {
        "It hit something it could not carry on from. What happened",
        "has been written down in the logs folder beside the game.",
        "",
        "Starting again puts you back at the main menu. Nothing that",
        "was saved is lost.",
    };
    const char* const* lines = died ? fell : stopped;
    const int line_count = died ? int(sizeof(fell) / sizeof(fell[0]))
                                : int(sizeof(stopped) / sizeof(stopped[0]));
    for (int i = 0; i < line_count; ++i) {
      dl->AddText(f, mid, ImVec2(tl.x + pad, y), kInk, lines[i]);
      y += mid * 1.35f;
    }
    y += mid * 0.4f;
    char foot[128];
    const int left = int(kGraceSeconds - (ImGui::GetTime() - since_)) + 1;
    std::snprintf(foot, sizeof(foot),
                  "Starting again in %d...      A to do it now      "
                  "B to keep waiting", left > 0 ? left : 0);
    dl->AddText(f, mid, ImVec2(tl.x + pad, y), kQuiet, foot);

    const Press p = JustPressed();
    if (p.wait) {
      g_stuck.store(false, std::memory_order_relaxed);
      REXLOG_INFO("stuck: the player chose to keep waiting");
      return;
    }
    if (!StillOpen()) return;
    if (p.go) {
      REXLOG_INFO("stuck: starting again at the player's request");
      NbaModRestart();
      return;
    }
    // Nobody has said anything. A game that has stopped may have taken the
    // controller down with it, so waiting for permission that cannot be given
    // would leave the player with a dead window and no way out of it.
    if (ImGui::GetTime() - since_ >= kGraceSeconds) {
      REXLOG_INFO("stuck: nothing was pressed in {} seconds - starting again",
                  int(kGraceSeconds));
      NbaModRestart();
    }
  }

 private:
  bool shown_ = false;
  double since_ = 0.0;
};

void ReportStall(int seconds) {
  REXLOG_ERROR("the game has not drawn a frame for {} seconds - it is stuck. "
               "What follows is where each part of it stopped.", seconds);
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return;
  THREADENTRY32 te{};
  te.dwSize = sizeof(te);
  const DWORD self = GetCurrentThreadId();
  const DWORD pid = GetCurrentProcessId();
  int shown = 0;
  if (Thread32First(snap, &te)) {
    do {
      if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
      std::vector<std::string> stack = StackOf(te.th32ThreadID);
      if (stack.empty()) continue;
      ++shown;
      REXLOG_ERROR("  thread {}{}:", te.th32ThreadID,
                   LooksLikeGuest(stack) ? " (game code)" : "");
      for (const std::string& name : stack) {
        REXLOG_ERROR("      {}", name);
      }
    } while (Thread32Next(snap, &te));
  }
  CloseHandle(snap);
  if (!shown) {
    REXLOG_ERROR("  nothing could be read back - no stacks were available.");
  }
  REXLOG_ERROR("The game is still open. The screen now offers to start it "
               "again; this log is worth keeping either way.");
}

// Windows' last word before a process goes. Everything else has had its
// chance at the fault by the time this runs, so the game really is finished.
// The thread that faulted is parked rather than allowed to unwind: the window
// and this port's own drawing live on other threads and carry on without it,
// which is what puts the way out on the screen.
LONG CALLBACK LastWord(EXCEPTION_POINTERS* info) {
  const DWORD code = info && info->ExceptionRecord
                         ? info->ExceptionRecord->ExceptionCode : 0;
  if (!StillOpen()) {
    return EXCEPTION_CONTINUE_SEARCH;   // on the way out anyway; let it go
  }
  REXLOG_ERROR("the game has fallen over ({:08X}) - it is finished, but the "
               "window is not. Offering to start it again.", code);
  g_fell_over.store(true, std::memory_order_relaxed);
  g_stuck.store(true, std::memory_order_relaxed);
  for (;;) {
    Sleep(1000);      // never come back; the rest of the process carries on
  }
}

void Watch() {
  const int stall = StallSeconds();
  if (stall <= 0) return;
  SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
  SymInitialize(GetCurrentProcess(), nullptr, TRUE);

  unsigned long long last = 0;
  int still = 0;            // seconds the counter has not moved
  bool ever_moved = false;
  int reports = 0;
  int since_death = 0;
  for (;;) {
    Sleep(1000);
    // If the game died and the panel has not managed to get anyone out of it
    // - the window may have gone down with it - do it from here instead.
    if (g_fell_over.load(std::memory_order_relaxed)) {
      if (!StillOpen()) return;
      if (++since_death > int(kGraceSeconds) + 15) {
        REXLOG_ERROR("stuck: nothing came of the panel - starting again");
        NbaModRestart();
        since_death = 0;
      }
      continue;
    }
    const unsigned long long now = NbaGuestFrameCount();
    if (now != last) {
      last = now;
      still = 0;
      ever_moved = true;
      g_stuck.store(false, std::memory_order_relaxed);
      continue;
    }
    if (!ever_moved) continue;       // never started; not our business
    if (!StillOpen()) return;        // the player closed it; nothing to say
    if (++still != stall) continue;
    still = 0;
    g_stuck.store(true, std::memory_order_relaxed);
    if (reports++ >= kMaxReports) continue;
    ReportStall(stall);
  }
}

}  // namespace

void NbaWatchdogCreate(rex::ui::ImGuiDrawer* drawer) {
  if (!drawer) return;
  // The drawer owns it from here; it lives as long as the window does.
  new WayOut(drawer);
}

void NbaWatchdogWatchWindow(void* hwnd) {
  g_window.store(hwnd, std::memory_order_relaxed);
}

void NbaStartWatchdog() {
  static bool started = false;
  if (started) return;
  started = true;
  // Last, so it is the filter that stands when the runtime has installed its
  // own; the crash report is written by a vectored handler, which runs first
  // either way. See src/crash_report.cpp.
  SetUnhandledExceptionFilter(&LastWord);
  std::thread(Watch).detach();
}
