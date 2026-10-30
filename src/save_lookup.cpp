// NBA JAM: On Fire Edition - finding the save the game made last time
//
// The game names each save after the moment it was made ("PROGRESS
// 20261030113223", "REAL_AI ..."), so to load one it has to ask which saves
// exist. It asks through sub_8254C8E8, a small wrapper that looks up the
// player's 64-bit id and hands it to XamContentAggregateCreateEnumerator.
//
// ReXGlue v0.10.0 reads every integer argument of a system call from the low
// 32 bits of its register (ArgTranslator::GetIntegerArgumentValue in
// rex/ppc/function.h), the 64-bit ones included. The id B13EBABEBABEBABE
// arrives as 00000000BABEBABE, the runtime lists the folder for a player who
// does not exist, and the game is told there are no saves. So it made a new
// one on every launch and played from scratch: progress was written every
// time and never read back. The saves themselves were fine.
//
// The runtime's table of system calls is its own, and the port cannot put a
// different function in it. What it does export is the typed function behind
// that entry, which takes the id as a real 64-bit value. So this hook sits on
// the instruction after the wrapper's call returns, and when the call
// succeeded with an id whose top half it would have lost, it makes the same
// call again with the whole id and gives the game that list instead. The empty
// one is released. Nothing else about the game's flow changes.
//
// Remove this when the runtime reads 64-bit arguments whole.

#include "save_lookup.h"

#include <cstring>

#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/object_table.h>
#include <rex/types.h>

#include "guest_base.h"

namespace rex::kernel::xam {
// Exported by rexruntime but declared in no public header.
u32 XamContentAggregateCreateEnumerator_entry(u64 xuid, u32 device_id,
                                              u32 content_type, u32 unk3,
                                              mapped_u32 handle_out);
}  // namespace rex::kernel::xam

void NbaSaveLookupFullId(PPCRegister& r1, PPCRegister& r3, PPCRegister& r26,
                         PPCRegister& r27, PPCRegister& r31) {
  if (r3.u32 != 0 || r26.u32 == 0) {
    return;  // the call failed; there is no list to replace
  }
  const uintptr_t base = NbaGuestBase();
  if (!base) {
    return;
  }
  // The wrapper keeps the id at r1+80, big-endian like everything the game
  // stores.
  uint64_t stored = 0;
  std::memcpy(&stored, reinterpret_cast<const void*>(base + r1.u32 + 80),
              sizeof(stored));
  const uint64_t xuid = __builtin_bswap64(stored);
  if ((xuid >> 32) == 0) {
    return;  // nothing was cut off
  }

  auto* handle_ptr = reinterpret_cast<rex::be_u32*>(base + r26.u32);
  const uint32_t empty_list = *handle_ptr;
  const uint32_t result = rex::kernel::xam::XamContentAggregateCreateEnumerator_entry(
      xuid, r27.u32, r31.u32, 0, mapped_u32(handle_ptr, r26.u32));
  if (result != 0) {
    *handle_ptr = empty_list;  // keep what the game had
    return;
  }
  if (auto* kernel = rex::system::kernel_state()) {
    kernel->object_table()->ReleaseHandle(empty_list);
  }
  r3.u64 = result;

  static bool said = false;
  if (!said) {
    said = true;
    REXLOG_INFO("saves: listing them for player {:016X} (the runtime would "
                "have looked under {:016X})",
                xuid, xuid & 0xFFFFFFFFull);
  }
}
