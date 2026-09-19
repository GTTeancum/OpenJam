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
// Off unless NBAJAM_APT_TRACE is set, so a normal run pays nothing.
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

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>

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

}  // namespace

void NbaAptBlock(PPCRegister& r4, PPCRegister& r6) {
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
  REXLOG_INFO("apt_trace: {:016x} len {}", print, len);
}
