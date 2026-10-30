// NBA JAM: On Fire Edition - where the guest's memory actually is
//
// The runtime maps the guest's 4 GB at 0x100000000 when it can and somewhere
// else when it cannot: Proton puts something there on a Steam Deck, and on
// Windows an overlay, an antivirus hook or a mod swap's predecessor can be
// holding it. Every "the game has fallen over" at startup reported so far had
// the memory at 0x200000000 while this port's own code was still reading
// 0x100000000.
//
// So nothing here assumes. The base is asked of the runtime once and kept,
// which makes reading it as cheap as the constant it replaces.

#pragma once

#include <atomic>
#include <cstdint>

namespace nba_detail {
extern std::atomic<uintptr_t> g_guest_base;
uintptr_t LookUpGuestBase();
}  // namespace nba_detail

// The host address of guest address zero, or 0 before the runtime has mapped
// the guest's memory - nothing of the game's runs before then.
inline uintptr_t NbaGuestBase() {
  const uintptr_t base =
      nba_detail::g_guest_base.load(std::memory_order_relaxed);
  return base ? base : nba_detail::LookUpGuestBase();
}

// Says where it landed, and warns when it is not where it usually is, so a
// player's log shows it without anyone having to ask.
void NbaReportGuestBase();
