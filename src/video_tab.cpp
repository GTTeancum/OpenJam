// NBA JAM: On Fire Edition - a Video tab in the game's own Settings screen
//
// Options > Settings is one screen with a row of tabs - Gameplay, Privileges
// and Audio from the main menu - and it holds no settings of its own. It asks
// the game's settings object (the Bounce_Settings natives, registered in
// sub_828A8E90) for everything: which tabs to show, the rows in each, the
// choices on each row, which choice is current, and to change one. This adds
// a fourth tab by answering those questions for it, so the rows, arrows,
// sounds and Restore Defaults are the screen's own and only the answers are
// the port's. The settings themselves are in src/video_settings.cpp.
//
// Tabs are numbered 0-7 in the game. The Video tab is 8, which nothing in the
// game knows about:
//
//   - The tab list (sub_822D7688) builds the numbers for where the screen was
//     opened from, then gives each a name from a table of eight. Wherever the
//     list has Audio (4) in it among others, 8 goes on the end; its name and
//     the line under the tabs are supplied, and the table is not read past
//     its end.
//   - A tab's rows (sub_822E2308) are counted first (sub_822DC750, which
//     says none for a tab it does not know, and is told five) and then built
//     by a switch with no case for 8. At the switch's common exit, tab 8 gets
//     its rows, built through the same calls the game's own builders make: an
//     `id`, a `control` of 0 (a list of choices), a `name`, and `canedit`.
//   - Choices (sub_822DCC58), the current choice (sub_822DD800), a change
//     (sub_822DD980) and Restore Defaults (sub_822DD628) are each taken over
//     at their first instruction for the Video rows' ids and Video's tab
//     number, and left alone for everything else.
//
// The rows are ids 0x7100 and up. The game reads ids below 15 as its own
// settings, 15-999 as Privileges and 1000 up as profile settings, which is
// why each of those methods is caught before it looks.
//
// Answering means calling the game: the screen's result objects are the
// game's, filled through their own virtual methods. A mid-ASM hook is handed
// references into the guest's register file, and r3 is the first thing in
// PPCContext, so the context is reached through it; the guest functions are
// then called through the module's dispatch table, as an indirect call in
// recompiled code would be. Text the game is given has to be in guest memory,
// so each string is copied into the guest heap once and reused.

#include "video_tab.h"

#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "guest_base.h"
#include "video_settings.h"

namespace {

constexpr uint32_t kVideoTab = 8;
constexpr uint32_t kAudioTab = 4;
constexpr uint32_t kFirstId = 0x7100;

// The module's layout, as generated/default/nbajam_ofe_pch.h has it: the
// dispatch table sits after the image, one host pointer per guest
// instruction slot.
constexpr uint64_t kImageBase = 0x82000000ull;
constexpr uint64_t kImageSize = 0xC50000ull;
constexpr uint64_t kCodeBase = 0x82200000ull;

// The tab names, indexed by tab number. Entry 4 is Audio's; Video borrows
// it for the lookup and then has its own name put in place of the result.
constexpr uint32_t kTabNameKeys = 0x82B2BAD0;

uint8_t* Base() { return reinterpret_cast<uint8_t*>(NbaGuestBase()); }

uint32_t Load32(uint32_t at) {
  uint32_t v;
  std::memcpy(&v, Base() + at, 4);
  return __builtin_bswap32(v);
}

PPCContext& Context(PPCRegister& r3) {
  return *reinterpret_cast<PPCContext*>(&r3);
}

// Call a guest function with up to four integer arguments; returns r3.
uint32_t Call(PPCContext& ctx, uint32_t fn, uint64_t a, uint64_t b = 0,
              uint64_t c = 0, uint64_t d = 0) {
  uint8_t* base = Base();
  PPCFunc* f = *reinterpret_cast<PPCFunc**>(
      base + kImageBase + kImageSize + (uint64_t(fn) - kCodeBase) * 2);
  if (!f) {
    REXLOG_ERROR("video tab: no function at {:08X}", fn);
    return 0;
  }
  ctx.r3.u64 = a;
  ctx.r4.u64 = b;
  ctx.r5.u64 = c;
  ctx.r6.u64 = d;
  f(ctx, base);
  return ctx.r3.u32;
}

// A virtual call on a guest object: `slot` is the byte offset in its vtable.
uint32_t Virtual(PPCContext& ctx, uint32_t obj, uint32_t slot, uint64_t b = 0,
                 uint64_t c = 0, uint64_t d = 0) {
  return Call(ctx, Load32(Load32(obj) + slot), obj, b, c, d);
}

// A string the game can read, made once and kept.
uint32_t Text(const std::string& s) {
  static std::mutex lock;
  static std::unordered_map<std::string, uint32_t> made;
  std::lock_guard<std::mutex> guard(lock);
  auto it = made.find(s);
  if (it != made.end()) return it->second;
  auto* kernel = rex::system::kernel_state();
  if (!kernel || !kernel->memory()) return 0;
  const uint32_t at = kernel->memory()->SystemHeapAlloc(uint32_t(s.size() + 1));
  if (!at) return 0;
  std::memcpy(Base() + at, s.c_str(), s.size() + 1);
  made.emplace(s, at);
  return at;
}

// The result objects' methods, as the game's builders use them.
constexpr uint32_t kSetCount = 4;   // (count) on a list of choices
constexpr uint32_t kSetBool = 8;    // (row, key, value) on rows
constexpr uint32_t kSetText = 8;    // (index, text) on a list of choices
constexpr uint32_t kSetInt = 12;    // (row, key, value) on rows; (value) on a single value
constexpr uint32_t kGetInt = 12;    // () on the value a change arrives with
constexpr uint32_t kSetString = 20; // (row, key, text) on rows

bool IsVideoRow(uint32_t id) {
  return id >= kFirstId && id < kFirstId + NbaVideoRows().size();
}

}  // namespace

void NbaVideoTabList(PPCRegister& r1, PPCRegister& r25) {
  const uint32_t list = r1.u32 + 96;
  const uint32_t n = r25.u32;
  if (n < 2 || n > 3) return;   // a single tab is a screen of its own
  bool audio = false;
  for (uint32_t i = 0; i < n; ++i) {
    audio = audio || Load32(list + 4 * i) == kAudioTab;
  }
  if (!audio) return;
  const uint32_t v = __builtin_bswap32(kVideoTab);
  std::memcpy(Base() + list + 4 * n, &v, 4);
  r25.u64 = n + 1;
}

void NbaVideoTabNameKey(PPCRegister& r5, PPCRegister& r28) {
  if (r28.u32 == kVideoTab) r5.u64 = Load32(kTabNameKeys + 4 * kAudioTab);
}

void NbaVideoTabName(PPCRegister& r6, PPCRegister& r28) {
  if (r28.u32 == kVideoTab) r6.u64 = Text("VIDEO");
}

void NbaVideoTabMessage(PPCRegister& r6, PPCRegister& r28) {
  if (r28.u32 == kVideoTab) {
    r6.u64 = Text("Resolution and sharpness change the next time the game starts.");
  }
}

void NbaVideoTabNoDescription(PPCRegister& r27, PPCRegister& r28) {
  if (r28.u32 == kVideoTab) r27.u64 = 0;
}

void NbaVideoTabRows(PPCRegister& r3, PPCRegister& r29, PPCRegister& r31) {
  if (r29.u32 != kVideoTab) return;
  PPCContext& ctx = Context(r3);
  const uint32_t out = r31.u32;
  const auto rows = NbaVideoRows();
  for (uint32_t i = 0; i < rows.size(); ++i) {
    Virtual(ctx, out, kSetInt, i, Text("id"), kFirstId + i);
    Virtual(ctx, out, kSetInt, i, Text("control"), 0);
    Virtual(ctx, out, kSetString, i, Text("name"), Text(rows[i].name));
    Virtual(ctx, out, kSetBool, i, Text("canedit"), 1);
  }
}

bool NbaVideoTabRowCount(PPCRegister& r3, PPCRegister& r4) {
  if (r4.u32 != kVideoTab) return false;
  r3.u64 = NbaVideoRows().size();
  return true;
}

bool NbaVideoTabChoices(PPCRegister& r3, PPCRegister& r4, PPCRegister& r6) {
  if (!IsVideoRow(r4.u32)) return false;
  PPCContext& ctx = Context(r3);
  const uint32_t out = r6.u32;
  const NbaVideoRow row = NbaVideoRows()[r4.u32 - kFirstId];
  Virtual(ctx, out, kSetCount, row.choices.size());
  for (uint32_t i = 0; i < row.choices.size(); ++i) {
    Virtual(ctx, out, kSetText, i, Text(row.choices[i]));
  }
  return true;
}

bool NbaVideoTabValue(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5) {
  if (!IsVideoRow(r4.u32)) return false;
  PPCContext& ctx = Context(r3);
  const uint32_t out = r5.u32;
  Virtual(ctx, out, kSetInt, uint32_t(NbaVideoRows()[r4.u32 - kFirstId].at));
  return true;
}

bool NbaVideoTabChange(PPCRegister& r3, PPCRegister& r4, PPCRegister& r6) {
  if (!IsVideoRow(r4.u32)) return false;
  PPCContext& ctx = Context(r3);
  const int row = int(r4.u32 - kFirstId);
  const int at = int(Virtual(ctx, r6.u32, kGetInt));
  NbaVideoSet(row, at);
  return true;
}

bool NbaVideoTabReset(PPCRegister& r4) {
  if (r4.u32 != kVideoTab) return false;
  NbaVideoReset();
  return true;
}
