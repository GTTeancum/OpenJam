// NBA JAM: On Fire Edition - corrected vector instructions
//
// ReXGlue v0.10.0 translates a few vector instructions by writing the result
// into the destination register one element at a time, while still reading the
// source registers. That is only safe when the destination is a different
// register. When it is not - `vpkuwus128 v62,v62,v61` - later elements are
// read after they have been overwritten, and half the result is garbage.
//
// This is what breaks the EA Sports intro video. The VP6 decoder's motion
// compensation filters end with a pack, all three `vpkuwus128` in the image
// have the destination as one of their sources, and there are no other uses of
// the instruction anywhere in the game, which is why nothing else misbehaves.
//
// tools/fix_vector_aliasing.py rewrites those instructions to call the
// versions below, which compute the whole result before storing any of it.
// See BUILDING.md.
//
// Registers are held byte-reversed by the SDK, so guest element k of a word
// vector is host `u32[3 - k]` and guest halfword k is host `u16[7 - k]`. The
// helpers here do that flip so the bodies read in guest order.

#pragma once

#include <rex/ppc/context.h>

#include <cstdint>

namespace rexfix {

inline uint32_t guest_word(const PPCVRegister& v, int i) { return v.u32[3 - i]; }
inline uint16_t guest_half(const PPCVRegister& v, int i) { return v.u16[7 - i]; }
inline void set_guest_half(PPCVRegister& v, int i, uint16_t x) { v.u16[7 - i] = x; }
inline void set_guest_byte(PPCVRegister& v, int i, uint8_t x) { v.u8[15 - i] = x; }

// vpkuwus vD,vA,vB - unsigned words to unsigned halfwords, saturating.
// Result halfwords 0..3 come from vA, 4..7 from vB.
inline void vpkuwus(PPCVRegister& d, const PPCVRegister& a, const PPCVRegister& b) {
  uint16_t r[8];
  for (int i = 0; i < 4; ++i) {
    uint32_t x = guest_word(a, i);
    r[i] = x > 0xFFFFu ? uint16_t(0xFFFF) : uint16_t(x);
  }
  for (int i = 0; i < 4; ++i) {
    uint32_t x = guest_word(b, i);
    r[4 + i] = x > 0xFFFFu ? uint16_t(0xFFFF) : uint16_t(x);
  }
  for (int i = 0; i < 8; ++i) set_guest_half(d, i, r[i]);
}

// vpkuhus vD,vA,vB - unsigned halfwords to unsigned bytes, saturating.
// Unused by this title, but the SDK builds it the same unsafe way.
inline void vpkuhus(PPCVRegister& d, const PPCVRegister& a, const PPCVRegister& b) {
  uint8_t r[16];
  for (int i = 0; i < 8; ++i) {
    uint16_t x = guest_half(a, i);
    r[i] = x > 0xFFu ? uint8_t(0xFF) : uint8_t(x);
  }
  for (int i = 0; i < 8; ++i) {
    uint16_t x = guest_half(b, i);
    r[8 + i] = x > 0xFFu ? uint8_t(0xFF) : uint8_t(x);
  }
  for (int i = 0; i < 16; ++i) set_guest_byte(d, i, r[i]);
}

}  // namespace rexfix
