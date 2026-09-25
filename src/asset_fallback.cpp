// NBA JAM: On Fire Edition - standing in for art a mod does not have
//
// src/null_page.cpp stops a missing asset killing the process. It does not
// make the game carry on: asked for a character that is not there, the match
// loader waits for something that will never arrive and the loading screen
// sits at nothing frames a second.
//
// sub_82367820 is the lookup that comes back empty. It has four ways of
// giving up and they all leave through the same door, with zero in r3. This
// sits in that doorway. When the answer is a real one it remembers it; when
// the answer is nothing, it hands back the last real one instead.
//
// The stand-in is another character of the same kind, so what a player sees
// is the wrong face on one player rather than a game that will not start.
// That is the trade being made here, deliberately: a mod with a hole in its
// art is playable, and the hole is visible rather than fatal.
//
// Set NBAJAM_NO_ASSET_FALLBACK=1 to turn it off and get the empty answer
// back, which is what to do when working out why something is missing.

#include "asset_fallback.h"

#include <windows.h>

#include <cstdint>

#include <rex/logging.h>

namespace {

constexpr uintptr_t kGuestVirtualBase = 0x0000000100000000ull;

bool Enabled() {
  static const bool on = [] {
    char buf[8]{};
    const DWORD n = GetEnvironmentVariableA("NBAJAM_NO_ASSET_FALLBACK", buf,
                                            sizeof(buf));
    return !(n > 0 && n < sizeof(buf) && buf[0] != '0');
  }();
  return on;
}

// Whether this guest address can be read at all.
bool Readable(uint32_t addr) {
  if (addr < 0x1000 || addr >= 0xC0000000) return false;
  MEMORY_BASIC_INFORMATION mbi{};
  const void* p = reinterpret_cast<const void*>(kGuestVirtualBase + addr);
  if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) {
    return false;
  }
  const DWORD ok = PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
                   PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY |
                   PAGE_EXECUTE_WRITECOPY;
  return (mbi.Protect & ok) && !(mbi.Protect & PAGE_GUARD);
}

// Whether a remembered pointer still leads somewhere with an object in it.
// A resource that has been freed since is worse than no resource at all.
bool Alive(uint32_t addr) {
  if (!Readable(addr)) return false;
  // Every one of these carries a pointer in its first word; a zero there
  // means whatever was here has gone.
  const uint8_t* q = reinterpret_cast<const uint8_t*>(kGuestVirtualBase + addr);
  const uint32_t first = (uint32_t(q[0]) << 24) | (uint32_t(q[1]) << 16) |
                         (uint32_t(q[2]) << 8) | uint32_t(q[3]);
  return first != 0;
}

}  // namespace

// The two keys in the handle: which group the thing belongs to, and which
// thing it is. Both are big-endian in guest memory.
uint32_t KeyAt(uint32_t handle, int at) {
  if (!Readable(handle + at)) return 0;
  const uint8_t* q = reinterpret_cast<const uint8_t*>(
      kGuestVirtualBase + handle + at);
  return (uint32_t(q[0]) << 24) | (uint32_t(q[1]) << 16) |
         (uint32_t(q[2]) << 8) | uint32_t(q[3]);
}

void NbaAssetFallback(PPCRegister& r3, PPCRegister& r31) {
  static uint32_t last_good = 0;
  static int said = 0;

  if (r3.u32 != 0) {
    last_good = r3.u32;
    return;
  }
  if (!Enabled() || !Alive(last_good)) {
    return;
  }
  r3.u32 = last_good;
  if (said < 40) {
    ++said;
    REXLOG_WARN("missing asset: group {:08X} item {:08X} is not there, "
                "standing in {:08X} instead - a mod is short of some art",
                KeyAt(r31.u32, 0), KeyAt(r31.u32, 4), last_good);
  }
}
