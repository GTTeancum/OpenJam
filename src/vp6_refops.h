// NBA JAM: On Fire Edition - independent reference implementations of the
// vector instructions the VP6 decoder uses.
//
// These exist to answer one question: is the green corruption in the EA Sports
// intro caused by ReXGlue mistranslating a vector instruction? Reading the
// SDK's implementations against the PowerPC spec found nothing, but reading is
// not testing. tools/patch_refops.py rewrites the emitted C++ for chosen
// opcodes inside one recompiled function to call these instead, so a rebuild
// and a rerun says yes or no.
//
// Written straight from the spec in terms of *guest* element numbering, with
// no SIMD and no cleverness, so that agreeing with the SDK means both are
// right rather than both sharing a trick.
//
// The one thing that has to be carried over from the SDK is its register
// representation: a guest vector is held byte-reversed, so
//
//     host u8[j]  == guest byte  15 - j
//     host u16[j] == guest halfword 7 - j   (value correct)
//     host u32[j] == guest word     3 - j   (value correct)
//
// The helpers below do that index flip, so the bodies read in guest order.

#pragma once

#include <rex/ppc/context.h>

#include <cmath>
#include <cstdint>
#include <cstring>

namespace vp6ref {

// Guest-order element access over the SDK's reversed storage.
inline uint8_t gb(const PPCVRegister& v, int i) { return v.u8[15 - i]; }
inline void sb(PPCVRegister& v, int i, uint8_t x) { v.u8[15 - i] = x; }
inline int16_t gh(const PPCVRegister& v, int i) { return v.s16[7 - i]; }
inline void sh(PPCVRegister& v, int i, int16_t x) { v.s16[7 - i] = x; }
inline int32_t gw(const PPCVRegister& v, int i) { return v.s32[3 - i]; }
inline uint32_t gwu(const PPCVRegister& v, int i) { return v.u32[3 - i]; }
inline void sw(PPCVRegister& v, int i, int32_t x) { v.s32[3 - i] = x; }
inline float gf(const PPCVRegister& v, int i) { return v.f32[3 - i]; }
inline void sf(PPCVRegister& v, int i, float x) { v.f32[3 - i] = x; }

inline int32_t sat32(int64_t x) {
  if (x > INT32_MAX) return INT32_MAX;
  if (x < INT32_MIN) return INT32_MIN;
  return int32_t(x);
}
inline int16_t sat16(int32_t x) {
  if (x > 32767) return 32767;
  if (x < -32768) return -32768;
  return int16_t(x);
}
inline uint8_t satu8(int32_t x) {
  if (x > 255) return 255;
  if (x < 0) return 0;
  return uint8_t(x);
}

//---------------------------------------------------------------------------
// Integer
//---------------------------------------------------------------------------

inline void vaddsws(PPCVRegister& d, const PPCVRegister& a, const PPCVRegister& b) {
  int32_t r[4];
  for (int i = 0; i < 4; ++i) r[i] = sat32(int64_t(gw(a, i)) + int64_t(gw(b, i)));
  for (int i = 0; i < 4; ++i) sw(d, i, r[i]);
}

inline void vsubsws(PPCVRegister& d, const PPCVRegister& a, const PPCVRegister& b) {
  int32_t r[4];
  for (int i = 0; i < 4; ++i) r[i] = sat32(int64_t(gw(a, i)) - int64_t(gw(b, i)));
  for (int i = 0; i < 4; ++i) sw(d, i, r[i]);
}

inline void vaddshs(PPCVRegister& d, const PPCVRegister& a, const PPCVRegister& b) {
  int16_t r[8];
  for (int i = 0; i < 8; ++i) r[i] = sat16(int32_t(gh(a, i)) + int32_t(gh(b, i)));
  for (int i = 0; i < 8; ++i) sh(d, i, r[i]);
}

// vsraw: shift amount is the low 5 bits of the corresponding word of vB.
inline void vsraw(PPCVRegister& d, const PPCVRegister& a, const PPCVRegister& b) {
  int32_t r[4];
  for (int i = 0; i < 4; ++i) r[i] = gw(a, i) >> (gwu(b, i) & 0x1F);
  for (int i = 0; i < 4; ++i) sw(d, i, r[i]);
}

inline void vsrw(PPCVRegister& d, const PPCVRegister& a, const PPCVRegister& b) {
  uint32_t r[4];
  for (int i = 0; i < 4; ++i) r[i] = gwu(a, i) >> (gwu(b, i) & 0x1F);
  for (int i = 0; i < 4; ++i) sw(d, i, int32_t(r[i]));
}

inline void vslw(PPCVRegister& d, const PPCVRegister& a, const PPCVRegister& b) {
  uint32_t r[4];
  for (int i = 0; i < 4; ++i) r[i] = gwu(a, i) << (gwu(b, i) & 0x1F);
  for (int i = 0; i < 4; ++i) sw(d, i, int32_t(r[i]));
}

// vpkshus: signed halfwords -> unsigned bytes, saturating.
// Result bytes 0..7 come from vA, bytes 8..15 from vB.
inline void vpkshus(PPCVRegister& d, const PPCVRegister& a, const PPCVRegister& b) {
  uint8_t r[16];
  for (int i = 0; i < 8; ++i) r[i] = satu8(gh(a, i));
  for (int i = 0; i < 8; ++i) r[8 + i] = satu8(gh(b, i));
  for (int i = 0; i < 16; ++i) sb(d, i, r[i]);
}

// vpkuwus: unsigned words -> unsigned halfwords, saturating.
//
// Every pack here computes into a local array before storing. That is not
// style: ReXGlue's build_vpkuwus writes straight into vD while still reading
// vA and vB, so an instruction whose destination is also a source - such as
// the `vpkuwus128 v62,v62,v61` in sub_824E6258 - reads lanes it has already
// overwritten.
inline void vpkuwus(PPCVRegister& d, const PPCVRegister& a, const PPCVRegister& b) {
  uint16_t r[8];
  for (int i = 0; i < 4; ++i) {
    uint32_t x = gwu(a, i);
    r[i] = x > 0xFFFFu ? uint16_t(0xFFFF) : uint16_t(x);
  }
  for (int i = 0; i < 4; ++i) {
    uint32_t x = gwu(b, i);
    r[4 + i] = x > 0xFFFFu ? uint16_t(0xFFFF) : uint16_t(x);
  }
  for (int i = 0; i < 8; ++i) sh(d, i, int16_t(r[i]));
}

// vpkswss: signed words -> signed halfwords, saturating.
inline void vpkswss(PPCVRegister& d, const PPCVRegister& a, const PPCVRegister& b) {
  int16_t r[8];
  for (int i = 0; i < 4; ++i) r[i] = sat16(gw(a, i));
  for (int i = 0; i < 4; ++i) r[4 + i] = sat16(gw(b, i));
  for (int i = 0; i < 8; ++i) sh(d, i, r[i]);
}

// vperm: result byte i = (vA || vB)[vC[i] & 0x1F].
inline void vperm(PPCVRegister& d, const PPCVRegister& a, const PPCVRegister& b,
                  const PPCVRegister& c) {
  uint8_t r[16];
  for (int i = 0; i < 16; ++i) {
    unsigned idx = gb(c, i) & 0x1F;
    r[i] = idx < 16 ? gb(a, int(idx)) : gb(b, int(idx - 16));
  }
  for (int i = 0; i < 16; ++i) sb(d, i, r[i]);
}

// vsldoi: (vA || vB) shifted left by sh bytes, leftmost 16 kept.
inline void vsldoi(PPCVRegister& d, const PPCVRegister& a, const PPCVRegister& b, int shb) {
  uint8_t r[16];
  for (int i = 0; i < 16; ++i) {
    int k = i + shb;
    r[i] = k < 16 ? gb(a, k) : gb(b, k - 16);
  }
  for (int i = 0; i < 16; ++i) sb(d, i, r[i]);
}

inline void vmrghw(PPCVRegister& d, const PPCVRegister& a, const PPCVRegister& b) {
  int32_t r[4] = {gw(a, 0), gw(b, 0), gw(a, 1), gw(b, 1)};
  for (int i = 0; i < 4; ++i) sw(d, i, r[i]);
}

inline void vmrglw(PPCVRegister& d, const PPCVRegister& a, const PPCVRegister& b) {
  int32_t r[4] = {gw(a, 2), gw(b, 2), gw(a, 3), gw(b, 3)};
  for (int i = 0; i < 4; ++i) sw(d, i, r[i]);
}

inline void vupkhsb(PPCVRegister& d, const PPCVRegister& a) {
  int16_t r[8];
  for (int i = 0; i < 8; ++i) r[i] = int16_t(int8_t(gb(a, i)));
  for (int i = 0; i < 8; ++i) sh(d, i, r[i]);
}

inline void vupklsb(PPCVRegister& d, const PPCVRegister& a) {
  int16_t r[8];
  for (int i = 0; i < 8; ++i) r[i] = int16_t(int8_t(gb(a, 8 + i)));
  for (int i = 0; i < 8; ++i) sh(d, i, r[i]);
}

inline void vor(PPCVRegister& d, const PPCVRegister& a, const PPCVRegister& b) {
  uint8_t r[16];
  for (int i = 0; i < 16; ++i) r[i] = uint8_t(gb(a, i) | gb(b, i));
  for (int i = 0; i < 16; ++i) sb(d, i, r[i]);
}

inline void vmrghb(PPCVRegister& d, const PPCVRegister& a, const PPCVRegister& b) {
  uint8_t r[16];
  for (int i = 0; i < 8; ++i) { r[2 * i] = gb(a, i); r[2 * i + 1] = gb(b, i); }
  for (int i = 0; i < 16; ++i) sb(d, i, r[i]);
}

// VMX128 encodes vupkhsh/vupklsh as vupkhsb128/vupklsb128 with operand 2 set
// to 0x60; ReXGlue's dispatcher rewrites them, and the disassembly comment
// still says "vupkhsb128 vD,vB,v96". Getting this wrong unpacks bytes where
// the guest unpacks halfwords.
inline void vupkhsh(PPCVRegister& d, const PPCVRegister& a) {
  int32_t r[4];
  for (int i = 0; i < 4; ++i) r[i] = int32_t(gh(a, i));
  for (int i = 0; i < 4; ++i) sw(d, i, r[i]);
}

inline void vupklsh(PPCVRegister& d, const PPCVRegister& a) {
  int32_t r[4];
  for (int i = 0; i < 4; ++i) r[i] = int32_t(gh(a, 4 + i));
  for (int i = 0; i < 4; ++i) sw(d, i, r[i]);
}

inline void vspltw(PPCVRegister& d, const PPCVRegister& a, int uimm) {
  int32_t x = gw(a, uimm & 3);
  for (int i = 0; i < 4; ++i) sw(d, i, x);
}

//---------------------------------------------------------------------------
// Floating point
//---------------------------------------------------------------------------

inline void vmulfp(PPCVRegister& d, const PPCVRegister& a, const PPCVRegister& b) {
  float r[4];
  for (int i = 0; i < 4; ++i) r[i] = gf(a, i) * gf(b, i);
  for (int i = 0; i < 4; ++i) sf(d, i, r[i]);
}

// vmaddfp vD,vA,vC,vB: vD = vA*vC + vB, fused - AltiVec rounds once, so a
// separate multiply and add can differ in the last bit.
inline void vmaddfp(PPCVRegister& d, const PPCVRegister& a, const PPCVRegister& c,
                    const PPCVRegister& b) {
  float r[4];
  for (int i = 0; i < 4; ++i) r[i] = std::fma(gf(a, i), gf(c, i), gf(b, i));
  for (int i = 0; i < 4; ++i) sf(d, i, r[i]);
}

// vrfiz: round each float toward zero.
inline void vrfiz(PPCVRegister& d, const PPCVRegister& a) {
  float r[4];
  for (int i = 0; i < 4; ++i) r[i] = std::trunc(gf(a, i));
  for (int i = 0; i < 4; ++i) sf(d, i, r[i]);
}

// vcfux: unsigned word -> float, divided by 2^uimm.
inline void vcuxwfp(PPCVRegister& d, const PPCVRegister& a, int uimm) {
  float r[4];
  const float scale = std::ldexp(1.0f, -uimm);
  for (int i = 0; i < 4; ++i) r[i] = float(gwu(a, i)) * scale;
  for (int i = 0; i < 4; ++i) sf(d, i, r[i]);
}

// vctuxs: float * 2^uimm -> unsigned word, round toward zero, saturating.
inline void vcfpuxws(PPCVRegister& d, const PPCVRegister& a, int uimm) {
  uint32_t r[4];
  const float scale = std::ldexp(1.0f, uimm);
  for (int i = 0; i < 4; ++i) {
    const float x = gf(a, i) * scale;
    if (std::isnan(x) || x <= 0.0f) {
      r[i] = 0;
    } else if (x >= 4294967296.0f) {
      r[i] = 0xFFFFFFFFu;
    } else {
      r[i] = uint32_t(std::trunc(x));
    }
  }
  for (int i = 0; i < 4; ++i) sw(d, i, int32_t(r[i]));
}

inline void vaddfp(PPCVRegister& d, const PPCVRegister& a, const PPCVRegister& b) {
  float r[4];
  for (int i = 0; i < 4; ++i) r[i] = gf(a, i) + gf(b, i);
  for (int i = 0; i < 4; ++i) sf(d, i, r[i]);
}

inline void vsubfp(PPCVRegister& d, const PPCVRegister& a, const PPCVRegister& b) {
  float r[4];
  for (int i = 0; i < 4; ++i) r[i] = gf(a, i) - gf(b, i);
  for (int i = 0; i < 4; ++i) sf(d, i, r[i]);
}

// vcfsx: signed word -> float, divided by 2^uimm.
inline void vcsxwfp(PPCVRegister& d, const PPCVRegister& a, int uimm) {
  float r[4];
  const float scale = std::ldexp(1.0f, -uimm);
  for (int i = 0; i < 4; ++i) r[i] = float(gw(a, i)) * scale;
  for (int i = 0; i < 4; ++i) sf(d, i, r[i]);
}

// vctsxs: float * 2^uimm -> signed word, round toward zero, saturating.
// NaN yields 0, which is what the hardware does.
inline void vcfpsxws(PPCVRegister& d, const PPCVRegister& a, int uimm) {
  int32_t r[4];
  const float scale = std::ldexp(1.0f, uimm);
  for (int i = 0; i < 4; ++i) {
    const float x = gf(a, i) * scale;
    if (std::isnan(x)) {
      r[i] = 0;
    } else if (x >= 2147483647.0f) {
      r[i] = INT32_MAX;
    } else if (x <= -2147483648.0f) {
      r[i] = INT32_MIN;
    } else {
      r[i] = int32_t(std::trunc(x));
    }
  }
  for (int i = 0; i < 4; ++i) sw(d, i, r[i]);
}

//---------------------------------------------------------------------------
// Memory
//---------------------------------------------------------------------------

constexpr uintptr_t kGuestBase = 0x0000000100000000ull;

inline void lvx(PPCVRegister& d, uint32_t ea) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(kGuestBase + (ea & ~0xFu));
  for (int i = 0; i < 16; ++i) sb(d, i, p[i]);
}

inline void stvx(const PPCVRegister& s, uint32_t ea) {
  uint8_t* p = reinterpret_cast<uint8_t*>(kGuestBase + (ea & ~0xFu));
  for (int i = 0; i < 16; ++i) p[i] = gb(s, i);
}

// lvlx: bytes from EA to the end of its aligned quadword, left justified,
// zeros after.
inline void lvlx(PPCVRegister& d, uint32_t ea) {
  const unsigned eb = ea & 0xF;
  const uint8_t* p = reinterpret_cast<const uint8_t*>(kGuestBase + (ea & ~0xFu));
  for (int i = 0; i < 16; ++i) sb(d, i, unsigned(i) < 16 - eb ? p[eb + i] : 0);
}

// lvrx: bytes from the start of the aligned quadword up to EA, right
// justified, zeros before. eb == 0 loads nothing.
inline void lvrx(PPCVRegister& d, uint32_t ea) {
  const unsigned eb = ea & 0xF;
  const uint8_t* p = reinterpret_cast<const uint8_t*>(kGuestBase + (ea & ~0xFu));
  for (int i = 0; i < 16; ++i) sb(d, i, unsigned(i) >= 16 - eb ? p[i - (16 - eb)] : 0);
}

// stvewx: stores the word element selected by the address.
inline void stvewx(const PPCVRegister& s, uint32_t ea) {
  const uint32_t addr = ea & ~3u;
  const int k = int((addr >> 2) & 3);
  uint8_t* p = reinterpret_cast<uint8_t*>(kGuestBase + addr);
  for (int i = 0; i < 4; ++i) p[i] = gb(s, k * 4 + i);
}

}  // namespace vp6ref
