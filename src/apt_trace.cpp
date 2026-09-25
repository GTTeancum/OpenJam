// NBA JAM: On Fire Edition - front-end script tracing
//
// The game has its own interface tracing, switched on in
// data/xenon/fe/config/profile.txt. It is no use here: the file-output path
// writes to `cache:`, which this runtime deliberately does not provide, and
// the console path never reaches DbgPrint, so nothing comes out either way.
//
// So trace the interpreter instead. sub_82330438 is the ActionScript
// execution loop: it takes the block to run in r4 and its length in r6, keeps
// a program counter on the stack, and dispatches one byte at a time through a
// jump table. Logging every block it is handed gives a picture of what a
// screen actually executed, and diffing that between a working file and an
// edited one shows where the edited one stops.
//
// This file has a second job, and it is the one that matters in a normal run:
// saying whether the main menu is on screen.
//
// Nothing else in the port can tell. The runtime sees a window and a pad; the
// game's own idea of which screen it is showing never crosses into it. The
// twenty-two framework functions that lead to the interpreter are the same on
// every screen, the objects they are called with are singletons with no name
// in them, and a screen's own handlers never come back through this hook at
// all.
//
// What does differ is which blocks of script get run. Walking the front end
// with the trace on and bucketing the blocks by screen shows a handful that
// the main menu runs every frame and Select Sides never runs at all. Those
// are listed below by the same fingerprint the trace prints, and seeing one
// is what "the main menu is up" means here. It costs a length comparison per
// block in the normal case, because only blocks of exactly the right size are
// ever hashed.
//
// It is a heartbeat rather than a latch on purpose: the answer goes stale a
// third of a second after the last sighting, so leaving the menu takes the
// mod chooser with it without anything having to notice that it happened.
//
// The rest of the file - the trace, the memory peeks, the guest call stacks -
// is off unless NBAJAM_APT_TRACE is set, so a normal run pays nothing.
//
//   NBAJAM_APT_TRACE=1        log the blocks the interpreter runs
//   NBAJAM_APT_TRACE_MAX=n    stop after n lines (default 4000)
//   NBAJAM_APT_TRACE_AFTER=s  ignore everything for the first s seconds
//
// The window matters. Comparing two whole runs does not work: the same file
// traced twice can differ by a factor of three in how many distinct blocks it
// reaches, because the runs get different distances through boot in the same
// wall-clock time. Restricting the trace to the seconds either side of the
// screen under test makes two runs comparable.

#include "apt_trace.h"

#include <windows.h>
#include <dbghelp.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include <rex/logging.h>

namespace {

bool Enabled() {
  static const bool on = [] {
    char buf[8]{};
    DWORD n = GetEnvironmentVariableA("NBAJAM_APT_TRACE", buf, sizeof(buf));
    return n > 0 && n < sizeof(buf) && buf[0] != '0';
  }();
  return on;
}

uint64_t StartAfterMs() {
  static const uint64_t ms = [] {
    char buf[16]{};
    DWORD n = GetEnvironmentVariableA("NBAJAM_APT_TRACE_AFTER", buf, sizeof(buf));
    return (n > 0 && n < sizeof(buf)) ? uint64_t(atoi(buf)) * 1000ull : 0ull;
  }();
  return ms;
}

uint32_t Budget() {
  static const uint32_t max = [] {
    char buf[16]{};
    DWORD n = GetEnvironmentVariableA("NBAJAM_APT_TRACE_MAX", buf, sizeof(buf));
    uint32_t v = (n > 0 && n < sizeof(buf)) ? uint32_t(atoi(buf)) : 0;
    return v ? v : 4000u;
  }();
  return max;
}

// Guest addresses move between runs, so a block has to be identified by what
// it contains rather than where it sits. A hash of its first bytes is stable,
// and the same hash can be computed offline from a screen's payload, which is
// what makes a trace line traceable back to a particular piece of a file.
constexpr uintptr_t kGuestVirtualBase = 0x0000000100000000ull;

uint64_t Fingerprint(uint32_t code, uint32_t len) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(kGuestVirtualBase + code);
  const uint32_t n = (len == 0 || len > 64) ? 64 : len;
  uint64_t h = 0xCBF29CE484222325ull;
  for (uint32_t i = 0; i < n; ++i) {
    h = (h ^ p[i]) * 0x100000001B3ull;
  }
  return h;
}

// A guest call stack at the moment a block runs. Recompiled functions are
// ordinary C++ functions named sub_<guest address>, so walking the native
// stack and resolving against this build's own symbols says which guest
// functions led here - which is how to find out what only the main menu
// calls. See the guest-debugging notes.
bool Stacks() {
  static const bool on = [] {
    char buf[8]{};
    DWORD n = GetEnvironmentVariableA("NBAJAM_APT_STACK", buf, sizeof(buf));
    return n > 0 && n < sizeof(buf) && buf[0] != '0';
  }();
  return on;
}

void DumpStack() {
  // Twice a second is plenty: the question is which functions a screen runs,
  // not how often, and a stack line is long.
  static uint64_t last = 0;
  const uint64_t now = GetTickCount64();
  if (now - last < 500) return;
  last = now;
  static std::once_flag once;
  std::call_once(once, [] {
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
  });
  void* frames[24]{};
  const USHORT n = RtlCaptureStackBackTrace(1, 24, frames, nullptr);
  alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
  auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
  sym->SizeOfStruct = sizeof(SYMBOL_INFO);
  sym->MaxNameLen = MAX_SYM_NAME;
  std::string line;
  for (USHORT i = 0; i < n; ++i) {
    DWORD64 disp = 0;
    if (SymFromAddr(GetCurrentProcess(), DWORD64(frames[i]), &disp, sym)) {
      line += " < ";
      line += sym->Name;
    }
  }
  REXLOG_INFO("apt_stack:{}", line);
}

bool Context() {
  static const bool on = [] {
    char buf[8]{};
    DWORD n = GetEnvironmentVariableA("NBAJAM_APT_CTX", buf, sizeof(buf));
    return n > 0 && n < sizeof(buf) && buf[0] != '0';
  }();
  return on;
}

// Whether the guest really has memory there. The guest's whole address space
// is reserved on the host, so a wild pointer reads as a valid host address
// and faults only when it is touched; this asks first.
bool Readable(uint32_t addr, uint32_t len) {
  if (addr < 0x1000 || addr >= 0xC0000000) return false;
  MEMORY_BASIC_INFORMATION mbi{};
  const void* p = reinterpret_cast<const void*>(kGuestVirtualBase + addr);
  if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
  if (mbi.State != MEM_COMMIT) return false;
  const DWORD ok = PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
                   PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY |
                   PAGE_EXECUTE_WRITECOPY;
  if (!(mbi.Protect & ok) || (mbi.Protect & PAGE_GUARD)) return false;
  const uintptr_t end = uintptr_t(mbi.BaseAddress) + mbi.RegionSize;
  return uintptr_t(p) + len <= end;
}

// Guest memory at an address, as bytes and as whatever text is in it. Used
// once, to find out what the interpreter knows about the movie it is running.
void Peek(const char* what, uint32_t addr) {
  if (!Readable(addr, 48)) return;
  const uint8_t* p = reinterpret_cast<const uint8_t*>(kGuestVirtualBase + addr);
  char hex[3 * 48 + 1]{};
  char txt[49]{};
  for (int i = 0; i < 48; ++i) {
    std::snprintf(hex + i * 3, 4, "%02X ", p[i]);
    txt[i] = (p[i] >= 32 && p[i] < 127) ? char(p[i]) : '.';
  }
  REXLOG_INFO("apt_ctx: {} {:08X} | {} | {}", what, addr, hex, txt);
  // And one level down: a pointer in the first words often leads to a name.
  for (int i = 0; i < 12; ++i) {
    const uint32_t v = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) |
                       (uint32_t(p[i * 4 + 2]) << 8) | uint32_t(p[i * 4 + 3]);
    if (!Readable(v, 48)) continue;
    const char* q = reinterpret_cast<const char*>(kGuestVirtualBase + v);
    int n = 0;
    while (n < 40 && q[n] >= 32 && q[n] < 127) ++n;
    if (n >= 4) {
      REXLOG_INFO("apt_ctx:    +{} -> {:08X} '{}'", i * 4, v,
                  std::string(q, size_t(n)));
    }
  }
}

// The blocks the main menu runs every frame and other screens do not. Length
// first: the fingerprint is over the first 64 bytes, or the whole block when
// it is shorter, so a block of the wrong length cannot match and does not
// need hashing.
struct Beat {
  uint32_t len;
  uint64_t print;
};

// Blocks only the main menu runs. Every other screen the front end can be on
// around it was checked and none of them run these: the title screen, the
// autosave notice that comes over the top of it, Select Sides, and Choose
// Teams. Plenty of other blocks looked main-menu-only against one of those
// and turned out not to be against another, which is why the list is short.
//
// They come round about twice a second rather than every frame, which is what
// the window below is sized for.
constexpr Beat kMainMenu[] = {
    {49, 0x6fcfbe41d0cb491eull},
    {153, 0x1636d85ef2047fbfull},
};

// And blocks the main menu runs *every frame*. On their own these are not
// enough - the title screen runs them too - but they stop the instant the
// menu goes away, where the two above carry on being true for a second after
// it. Both have to be fresh, which is what makes the panel appear only on the
// menu and leave the moment the screen changes.
constexpr Beat kEveryFrame[] = {
    {14, 0xde21a435f627dbc2ull},
    {18, 0xacef04132dcd6cbfull},
};

// How long each sighting is good for. The menu's own blocks come round about
// twice a second, so they get a couple of seconds; the per-frame ones get a
// tenth, which is what makes leaving the screen take the panel with it
// immediately.
constexpr uint64_t kMenuGoodForMs = 2500;
constexpr uint64_t kFrameGoodForMs = 120;

std::atomic<uint64_t> g_menu_seen{0};
std::atomic<uint64_t> g_frame_seen{0};

bool Match(const Beat* beats, size_t count, uint32_t code, uint32_t len,
           uint64_t* print, bool* hashed) {
  for (size_t i = 0; i < count; ++i) {
    if (beats[i].len != len) continue;
    if (!*hashed) {
      *print = Fingerprint(code, len);
      *hashed = true;
    }
    if (beats[i].print == *print) return true;
  }
  return false;
}

void Watch(uint32_t code, uint32_t len) {
  uint64_t print = 0;
  bool hashed = false;
  const uint64_t now = GetTickCount64();
  if (Match(kMainMenu, sizeof(kMainMenu) / sizeof(*kMainMenu), code, len,
            &print, &hashed)) {
    g_menu_seen.store(now, std::memory_order_relaxed);
  }
  if (Match(kEveryFrame, sizeof(kEveryFrame) / sizeof(*kEveryFrame), code, len,
            &print, &hashed)) {
    g_frame_seen.store(now, std::memory_order_relaxed);
  }
}

}  // namespace

bool NbaFrontEndOnMainMenu() {
  const uint64_t menu = g_menu_seen.load(std::memory_order_relaxed);
  const uint64_t frame = g_frame_seen.load(std::memory_order_relaxed);
  if (menu == 0 || frame == 0) return false;
  const uint64_t now = GetTickCount64();
  return now - menu < kMenuGoodForMs && now - frame < kFrameGoodForMs;
}

void NbaAptBlock(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5,
                 PPCRegister& r6) {
  Watch(r4.u32, r6.u32 & 0x7FFFFFFF);
  if (!Enabled()) {
    return;
  }
  static std::mutex lock;
  static uint32_t seen = 0;
  static uint64_t last_print = 0;

  static const uint64_t started = GetTickCount64();
  if (GetTickCount64() - started < StartAfterMs()) {
    return;
  }

  std::lock_guard<std::mutex> guard(lock);
  const uint32_t len = r6.u32 & 0x7FFFFFFF;
  const uint64_t print = Fingerprint(r4.u32, len);
  // The loop is re-entered constantly with the same block while a screen
  // ticks; only the changes are interesting.
  if (print == last_print) {
    return;
  }
  last_print = print;
  if (seen >= Budget()) {
    return;
  }
  if (++seen == Budget()) {
    REXLOG_INFO("apt_trace: budget reached, no more blocks will be logged");
    return;
  }
  REXLOG_INFO("apt_trace: {:016x} len {} r3 {:08X} r5 {:08X}", print, len,
              r3.u32, r5.u32);
  // What the interpreter was handed alongside the block. Somewhere in there
  // is which movie is running, which is the thing the mod chooser needs.
  if (Context()) {
    Peek("r3", r3.u32);
    Peek("r5", r5.u32);
  }
  if (Stacks()) {
    DumpStack();
  }
}
