// NBA JAM: On Fire Edition - VP6 decoder instrumentation
//
// The EA Sports intro is a VP6 video in EA's own container
// (data/common/fmv/EASbrand_eng_HD_W.vp6, 1280x720, 109 frames). It plays
// with heavy green corruption. Comparing a captured frame against the same
// frame decoded by ffmpeg shows the clean areas matching to within rounding,
// so the texture upload, the YUV->RGB conversion and the scaling are all
// right: the decoded frame itself is wrong. The corruption is aligned to
// 16-pixel macroblock rows, which puts it inside the decoder.
//
// The decoder is guest code with no symbols. Two functions were found by
// ranking every recompiled function by its use of *integer* vector
// instructions - a game's vector math is overwhelmingly floating point, so a
// codec stands out sharply:
//
//   sub_824E6310  3022 instructions, the coefficient decoder and IDCT
//   sub_824E5FD8   113 instructions, writes one reconstructed 8x8 block
//
// sub_824E5FD8(r3 = destination, r4 = 8x8 block of signed 16-bit residuals,
// r5 = plane stride) adds a constant bias vector, saturates to unsigned
// bytes, and stores eight bytes per row with stvewx. It has two code paths
// selected on r3 & 0xF, because stvewx picks its vector element from the
// destination address; both compute the same thing.
//
// Everything here is off unless switched on from the environment, so a normal
// run is untouched:
//
//   NBAJAM_VP6_GEOMETRY=1        report the destination addresses and strides
//                                it is called with, to locate the frame planes
//   NBAJAM_VP6_DUMP_AT=<n>       at call n, write the guest memory spanned by
//                                those planes to vp6_planes_<n>.bin
//   NBAJAM_VP6_DUMP_MS=<ms>      dump every <ms> of decoding, up to six times.
//                                Call counts per frame vary with how much of a
//                                frame is inter-coded, so wall time is the only
//                                reliable way to land on a moment when the
//                                screen is visibly corrupt.
//   NBAJAM_VP6_NATIVE_BLOCK=1    replace sub_824E5FD8 with the C++ below
//
// The replacement is a bisection: if the corruption is unchanged with it in
// place, the fault is upstream in sub_824E6310 and this function is clean.

#include "vp6_hooks.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr uintptr_t kGuestVirtualBase = 0x0000000100000000ull;

template <typename T>
T* GuestPtr(uint32_t addr) {
  return reinterpret_cast<T*>(kGuestVirtualBase + addr);
}

int16_t LoadBE16(const uint8_t* p) {
  return int16_t((uint16_t(p[0]) << 8) | uint16_t(p[1]));
}

bool EnvOn(const char* name) {
  char buf[16]{};
  DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
  return n > 0 && n < sizeof(buf) && buf[0] != '0';
}

uint64_t EnvNum(const char* name) {
  char buf[32]{};
  DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
  return (n > 0 && n < sizeof(buf)) ? strtoull(buf, nullptr, 0) : 0;
}

const bool g_native = EnvOn("NBAJAM_VP6_NATIVE_BLOCK");
const bool g_geometry = EnvOn("NBAJAM_VP6_GEOMETRY");
const uint64_t g_dump_at = EnvNum("NBAJAM_VP6_DUMP_AT");
const uint64_t g_dump_ms = EnvNum("NBAJAM_VP6_DUMP_MS");

std::string SidecarPath(const std::string& name) {
  char exe[MAX_PATH]{};
  GetModuleFileNameA(nullptr, exe, MAX_PATH);
  std::string p(exe);
  size_t slash = p.find_last_of("\\/");
  return (slash == std::string::npos ? std::string() : p.substr(0, slash + 1)) + name;
}

//---------------------------------------------------------------------------
// Geometry recording
//
// Every call carries a destination address and a stride. Grouping the
// destinations by stride gives the address range of each plane, which is what
// is needed to pull a whole decoded frame out of guest memory and diff it
// against a reference decode.
//---------------------------------------------------------------------------

struct PlaneRange {
  uint32_t lo = 0xFFFFFFFFu;
  uint32_t hi = 0;
  uint64_t calls = 0;
};

std::mutex g_mutex;
std::map<uint32_t, PlaneRange> g_by_stride;  // stride -> observed range
uint64_t g_calls = 0;
uint64_t g_first_tick = 0;
int g_dumps = 0;
bool g_reported = false;
bool g_dumped = false;

void WriteGeometryReport() {
  FILE* f = nullptr;
  if (fopen_s(&f, SidecarPath("vp6_geometry.txt").c_str(), "w") != 0 || !f) {
    return;
  }
  fprintf(f, "sub_824E5FD8 destinations after %llu calls\n\n",
          static_cast<unsigned long long>(g_calls));
  fprintf(f, "%10s %12s %12s %12s %12s\n", "stride", "lo", "hi", "calls", "span");
  for (const auto& [stride, r] : g_by_stride) {
    fprintf(f, "%10u 0x%08X 0x%08X %12llu %12u\n", stride, r.lo, r.hi,
            static_cast<unsigned long long>(r.calls), r.hi - r.lo);
  }
  fclose(f);
}

// Dump every byte between the lowest and highest destination seen, so a
// reference decode can be diffed against it without knowing the exact plane
// layout in advance. The header records the strides so the dump can be
// unpacked afterwards.
void WritePlaneDump(uint64_t call) {
  uint32_t lo = 0xFFFFFFFFu, hi = 0;
  for (const auto& [stride, r] : g_by_stride) {
    lo = std::min(lo, r.lo);
    hi = std::max(hi, r.hi + stride * 8);
  }
  if (lo >= hi) {
    return;
  }
  const size_t len = size_t(hi - lo);
  if (len > 64u * 1024 * 1024) {
    return;  // implausible; do not try to write it
  }

  char name[64];
  snprintf(name, sizeof(name), "vp6_planes_%llu.bin", static_cast<unsigned long long>(call));
  FILE* f = nullptr;
  if (fopen_s(&f, SidecarPath(name).c_str(), "wb") != 0 || !f) {
    return;
  }
  // 64-byte text header, then the raw bytes.
  char hdr[64]{};
  snprintf(hdr, sizeof(hdr), "VP6DUMP base=%08X len=%zu strides=%zu", lo, len, g_by_stride.size());
  fwrite(hdr, 1, sizeof(hdr), f);
  fwrite(GuestPtr<const uint8_t>(lo), 1, len, f);
  fclose(f);

  FILE* t = nullptr;
  if (fopen_s(&t, SidecarPath(std::string(name) + ".txt").c_str(), "w") == 0 && t) {
    fprintf(t, "base 0x%08X  len %zu  at call %llu\n", lo, len,
            static_cast<unsigned long long>(call));
    for (const auto& [stride, r] : g_by_stride) {
      fprintf(t, "stride %u  lo 0x%08X (+%u)  hi 0x%08X (+%u)  calls %llu\n", stride, r.lo,
              r.lo - lo, r.hi, r.hi - lo, static_cast<unsigned long long>(r.calls));
    }
    fclose(t);
  }
}

void Observe(uint32_t dst, uint32_t stride) {
  std::lock_guard lock(g_mutex);
  ++g_calls;
  auto& r = g_by_stride[stride];
  r.lo = std::min(r.lo, dst);
  r.hi = std::max(r.hi, dst);
  ++r.calls;

  if (g_first_tick == 0) {
    g_first_tick = GetTickCount64();
  }
  if (!g_dumped && g_dump_at && g_calls >= g_dump_at) {
    g_dumped = true;
    WritePlaneDump(g_calls);
  }
  if (g_dump_ms && g_dumps < 6 &&
      GetTickCount64() - g_first_tick >= g_dump_ms * uint64_t(g_dumps + 1)) {
    ++g_dumps;
    WritePlaneDump(g_calls);
  }
  // Rewrite the geometry report as it goes; the last version written is the
  // most complete, and the process is normally killed rather than exiting.
  if (g_calls % 20000 == 0) {
    g_reported = true;
    WriteGeometryReport();
  }
}

}  // namespace

//---------------------------------------------------------------------------
// sub_824E5FD8(dst, block, stride)
//
//   for row in 0..7, col in 0..7:
//     dst[row*stride + col] = clamp_u8( sat_s16( block[row*8+col] + bias[col] ) )
//
// bias is a 16-byte constant in the image at 0x8209E7C0, eight signed
// halfwords. It is read from guest memory rather than hardcoded so this stays
// honest about what the guest actually uses.
//---------------------------------------------------------------------------
bool NbaVp6WriteBlock(PPCRegister& r3, PPCRegister& r4, PPCRegister& r5) {
  const uint32_t dst = r3.u32;
  const uint32_t block = r4.u32;
  const uint32_t stride = r5.u32;

  if (g_geometry || g_dump_at || g_dump_ms) {
    Observe(dst, stride);
  }
  if (!g_native) {
    return false;  // let the guest function run
  }

  static int16_t bias[8];
  static bool bias_loaded = false;
  if (!bias_loaded) {
    const uint8_t* p = GuestPtr<const uint8_t>(0x8209E7C0);
    for (int i = 0; i < 8; ++i) {
      bias[i] = LoadBE16(p + i * 2);
    }
    bias_loaded = true;
  }

  const uint8_t* src = GuestPtr<const uint8_t>(block);
  uint8_t* out = GuestPtr<uint8_t>(dst);

  for (int row = 0; row < 8; ++row) {
    for (int col = 0; col < 8; ++col) {
      int32_t v = int32_t(LoadBE16(src + (row * 8 + col) * 2)) + int32_t(bias[col]);
      // vaddshs saturates to signed 16 bits, then vpkshus saturates to
      // unsigned 8. The first clamp only matters for absurd coefficients but
      // it is what the hardware does.
      v = std::clamp(v, -32768, 32767);
      out[row * stride + col] = uint8_t(std::clamp(v, 0, 255));
    }
  }
  return true;  // handled here; skip the guest body
}
