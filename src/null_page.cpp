// NBA JAM: On Fire Edition - surviving a missing asset
//
// A mod that is missing a piece of art used to take the game down. The
// Legends mod ships a team whose second player has no portrait: the front end
// draws its missing-texture placeholder, which is what the console does too,
// and then the match starts, the renderer asks for that player's character,
// gets nothing back, and reads through it.
//
// The game is not careless about this. The code that crashed reads a pointer,
// reads a field 24 bytes into it, and then immediately checks whether what it
// got was zero and takes a "not there" path if it was. It survives a missing
// field perfectly well. What it cannot survive is the pointer itself being
// null, because on the console that read is a fault.
//
// So give address zero a page. Sixty-four kilobytes of zeros at the bottom of
// the guest's address space means a read through a null pointer returns what
// the game's own check is looking for, and the "not there" path it already
// has runs instead of the process dying. Every crash of this kind seen in
// this port - a missing character, a missing render node, a missing texture -
// has been a read within the first few hundred bytes of null, so one page
// covers all of them, and any future mod with a hole in its art degrades to
// a placeholder rather than a crash.
//
// What this does not do is hide a real bug: a write through null still lands
// in the page and is still wrong, so the diagnostics below say when the page
// has been touched at all. Set NBAJAM_NO_NULL_PAGE=1 to leave address zero
// unmapped and get the crash back, which is what to do when tracking down
// where a null came from rather than surviving it.

#include "null_page.h"

#include <windows.h>

#include <cstdint>

#include <rex/logging.h>

namespace {

constexpr uintptr_t kGuestVirtualBase = 0x0000000100000000ull;

// Generous: the reads that have killed this port landed between 0x18 and
// 0x2AC, and a structure with a large field offset could reach further.
constexpr SIZE_T kNullPageSize = 64 * 1024;

bool Wanted() {
  char buf[8]{};
  const DWORD n = GetEnvironmentVariableA("NBAJAM_NO_NULL_PAGE", buf,
                                          sizeof(buf));
  const bool off = n > 0 && n < sizeof(buf) && buf[0] != '0';
  return !off;
}

}  // namespace

void NbaMapNullPage() {
  if (!Wanted()) {
    REXLOG_INFO("null page: not mapped, as asked - a read through null will "
                "crash");
    return;
  }
  void* at = reinterpret_cast<void*>(kGuestVirtualBase);
  MEMORY_BASIC_INFORMATION mbi{};
  const bool known = VirtualQuery(at, &mbi, sizeof(mbi)) != 0;
  REXLOG_INFO("null page: guest zero is state {:X} protect {:X} over {} KB",
              known ? mbi.State : 0u, known ? mbi.Protect : 0u,
              known ? mbi.RegionSize / 1024 : 0u);
  const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
                         PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY |
                         PAGE_EXECUTE_WRITECOPY;
  if (known && mbi.State == MEM_COMMIT) {
    if (mbi.Protect & readable) {
      REXLOG_INFO("null page: already there");
      return;
    }
    // Committed but unreachable - the arena is laid out in one go and the
    // pages nothing should touch are left without access. Opening this one
    // is all that is needed.
    DWORD was = 0;
    if (VirtualProtect(at, kNullPageSize, PAGE_READWRITE, &was)) {
      REXLOG_INFO("null page: opened {} KB at guest zero (was protect {:X})",
                  kNullPageSize / 1024, was);
    } else {
      REXLOG_WARN("null page: could not open guest zero ({})", GetLastError());
    }
    return;
  }
  // The runtime reserves the whole arena, so this is a commit inside it
  // rather than a reservation of its own.
  void* got = VirtualAlloc(at, kNullPageSize, MEM_COMMIT, PAGE_READWRITE);
  if (!got) {
    REXLOG_WARN("null page: could not map address zero ({}); a missing asset "
                "will take the game down", GetLastError());
    return;
  }
  // VirtualAlloc hands back zeroed pages, which is the whole point: a read
  // through a null pointer now returns the zero that the game's own checks
  // are testing for.
  REXLOG_INFO("null page: {} KB mapped at guest zero, so a missing asset "
              "degrades rather than crashes", kNullPageSize / 1024);
}
