// NBA JAM: On Fire Edition - DDS texture replacement
//
// Mods for this game are made in DDS. The PlayStation 3 release effectively
// stores DDS already - a short EA header over a plain linear DXT mip chain -
// while this Xbox 360 build stores XPR2 resources whose pixel data is tiled
// into the GPU's swizzled layout and byte swapped in 16-bit pairs. Handing the
// 360 build a PS3 archive gets you black menus: it rejects the load quietly.
//
// Rather than converting mod textures to the 360 layout ahead of time, the
// port accepts DDS directly. A mod drops `.dds` files into
//
//     <game_data_root>/dlc_textures/<TEXTURE_NAME>.dds
//     <game_data_root>/dlc_textures/<16-HEX-DIGIT HASH>.dds
//
// and they replace the matching textures at load time. Nothing is written to
// disk and the original archives are untouched.
//
// Two ways of naming a file because one is not enough. Textures in the ge_*
// archives carry real names - `adi_logo_tex`, `ad_rock_name_tex` - and naming
// a replacement after one is the obvious thing to do. The front-end and boot
// archives do not: every texture in them is called `strName`, so the title
// logo and the ESRB screen are indistinguishable by name. Those are matched
// instead on a hash of the pixel data they are replacing, which is unique and
// needs no cooperation from the archive. tools/ast_textures.py prints the
// hashes.
//
// How it works. sub_823FC250 is the guest function that recognises a texture
// resource; it starts by comparing the first four bytes against 'XPR2'. This
// hook runs at its entry, before that check, and edits the blob in place.
//
// An XPR2 blob is:
//
//     +0x00  'XPR2'
//     +0x04  u32  header end
//     +0x08  u32  pixel data size
//     +0x0C  u32  resource count
//     +0x10  'TX2D'
//     +0x14  u32  base of the resource record
//     +0x24  char[] lower-case texture name
//
// The two offsets that matter are not where they first appear to be, so both
// are taken from what the guest itself does rather than from the shape of one
// example:
//
//     pixel data  at  BE32(+0x04) + 12   - the guest computes exactly this
//     descriptor  at  BE32(+0x14) + 40
//
// Checked against 330 textures across the HUD, front-end and boot archives
// with no failures. Assuming the descriptor is simply the last 24 bytes of
// the header happens to be right for the common 112-byte case and wrong for
// the rest, and the pixel data is never at BE32(+0x04) - it is 12 bytes later.
//
// Three descriptor fields are what matter here:
//
//     dword0 bit 31      tiled
//     dword0 bits 22-30  row pitch in pixels >> 5
//     dword1 bits 0-5    format: 18 DXT1, 19 DXT2/3, 20 DXT4/5
//     dword1 bits 6-7    endianness: 0 none, 1 swap 8-in-16
//     dword2             (width-1) | (height-1) << 13
//     dword4 bits 6-9    highest mip level
//
// Clearing `tiled` and `endianness` is what lets the DDS blocks be used
// exactly as they are, with no rearranging and no byte swapping. The one
// constraint the hardware imposes on a linear texture is that each row of
// blocks starts on a 256-byte boundary, so rows are copied one at a time with
// padding rather than in a single memcpy.
//
// Only the base mip level is used. Linear mip chains have their own packing
// rules, and a replacement texture that does not minify is a far smaller
// problem than one that does not appear.

#include "dds_textures.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>

namespace {

constexpr uintptr_t kGuestVirtualBase = 0x0000000100000000ull;

// The GPU requires each row of blocks in a linear texture to begin on a
// 256-byte boundary.
constexpr uint32_t kLinearRowAlignment = 256;

// Xenos TextureFormat values.
constexpr uint32_t kFormatDXT1 = 18;
constexpr uint32_t kFormatDXT2_3 = 19;
constexpr uint32_t kFormatDXT4_5 = 20;

uint8_t* GuestPtr(uint32_t addr) {
  return reinterpret_cast<uint8_t*>(kGuestVirtualBase + addr);
}

uint32_t LoadBE16(const uint8_t* p) {
  return (uint32_t(p[0]) << 8) | p[1];
}

uint32_t LoadBE32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
         (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

void StoreBE32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}

// FNV-1a, over the original pixel data. tools/ast_textures.py computes the
// same value, so a replacement can be named after the texture it replaces.
uint64_t HashPixels(const uint8_t* p, size_t n) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 1099511628211ull;
  }
  return h;
}

uint32_t LoadLE32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
         (uint32_t(p[3]) << 24);
}

struct Dds {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format = 0;    // Xenos TextureFormat
  uint32_t block = 0;     // bytes per 4x4 block
  const uint8_t* data = nullptr;
  size_t data_size = 0;
  std::vector<uint8_t> storage;
};

bool ParseDds(std::vector<uint8_t>&& bytes, Dds& out) {
  if (bytes.size() < 128 || std::memcmp(bytes.data(), "DDS ", 4) != 0) {
    return false;
  }
  out.storage = std::move(bytes);
  const uint8_t* b = out.storage.data();
  out.height = LoadLE32(b + 12);
  out.width = LoadLE32(b + 16);

  const uint32_t pf_flags = LoadLE32(b + 80);
  if (!(pf_flags & 0x4)) {
    return false;  // not FourCC; uncompressed DDS is not supported
  }
  const char* fourcc = reinterpret_cast<const char*>(b + 84);
  if (!std::memcmp(fourcc, "DXT1", 4)) {
    out.format = kFormatDXT1;
    out.block = 8;
  } else if (!std::memcmp(fourcc, "DXT2", 4) || !std::memcmp(fourcc, "DXT3", 4)) {
    out.format = kFormatDXT2_3;
    out.block = 16;
  } else if (!std::memcmp(fourcc, "DXT4", 4) || !std::memcmp(fourcc, "DXT5", 4)) {
    out.format = kFormatDXT4_5;
    out.block = 16;
  } else {
    return false;
  }
  out.data = out.storage.data() + 128;
  out.data_size = out.storage.size() - 128;
  return out.width > 0 && out.height > 0;
}

//---------------------------------------------------------------------------
// The replacement set, discovered once from <game_data_root>/dlc_textures.
//---------------------------------------------------------------------------

std::once_flag g_scan_once;
std::unordered_map<std::string, std::filesystem::path> g_available;
std::mutex g_cache_mutex;
std::unordered_map<std::string, Dds> g_cache;
bool g_any = false;

// Set NBAJAM_DLC_TEXTURE_TRACE=1 to log every texture the guest loads, with
// both of the names a replacement could use. That is how you find the key for
// a texture you want to replace, and how you check the hook is firing at all.
bool TraceEnabled() {
  static const bool on = [] {
    char buf[8]{};
    DWORD n = GetEnvironmentVariableA("NBAJAM_DLC_TEXTURE_TRACE", buf, sizeof(buf));
    return n > 0 && n < sizeof(buf) && buf[0] != '0';
  }();
  return on;
}

std::string Lower(std::string s) {
  for (char& c : s) {
    c = char(std::tolower(static_cast<unsigned char>(c)));
  }
  return s;
}

void Scan() {
  std::string root = REXCVAR_GET(game_data_root);
  if (root.empty()) {
    return;
  }
  std::error_code ec;
  const std::filesystem::path dir = std::filesystem::path(root) / "dlc_textures";
  if (!std::filesystem::is_directory(dir, ec)) {
    return;
  }
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (!entry.is_regular_file(ec)) {
      continue;
    }
    const auto& p = entry.path();
    if (Lower(p.extension().string()) != ".dds") {
      continue;
    }
    g_available.emplace(Lower(p.stem().string()), p);
  }
  g_any = !g_available.empty();
  if (g_any) {
    REXLOG_INFO("dlc_textures: {} replacement texture(s) in {}", g_available.size(),
                dir.string());
  }
}

const Dds* Lookup(const std::string& name) {
  std::call_once(g_scan_once, Scan);
  if (!g_any) {
    return nullptr;
  }
  std::lock_guard lock(g_cache_mutex);
  auto cached = g_cache.find(name);
  if (cached != g_cache.end()) {
    return cached->second.width ? &cached->second : nullptr;
  }
  auto found = g_available.find(name);
  if (found == g_available.end()) {
    g_cache.emplace(name, Dds{});  // negative result, so we only look once
    return nullptr;
  }
  std::ifstream f(found->second, std::ios::binary);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
  Dds dds;
  if (!ParseDds(std::move(bytes), dds)) {
    REXLOG_WARN("dlc_textures: {} is not a DXT1/3/5 .dds; ignoring",
                   found->second.string());
    g_cache.emplace(name, Dds{});
    return nullptr;
  }
  auto [it, _] = g_cache.emplace(name, std::move(dds));
  return &it->second;
}

}  // namespace

// A PS3 archive stores textures as plain DDS files. This build's loader only
// recognises XPR2, so it rejects them - which is why dropping a PS3 .ast in
// leaves the menus black.
//
// Nothing has to be repacked to fix that. A DDS file opens with a 128-byte
// header, and an XPR2 header for a single texture fits in 120, so the XPR2
// form can be written straight over it and the pixel data left exactly where
// it already is. The layout below mirrors what the real archives use, with
// the resource record placed to fit:
//
//     +0x00  'XPR2'
//     +0x04  116          pixel data at 116 + 12 = 128, where the DDS put it
//     +0x08  data size
//     +0x0C  1            one resource
//     +0x10  'TX2D'
//     +0x14  56           resource record base
//     +0x24  name
//     +56+12 3, +56+16 1, +56+32 and +56+36 0xFFFF0000
//     +56+40 descriptor, ending at 120
//
// The one thing that must line up is the hardware's requirement that each row
// of blocks in a linear texture start on a 256-byte boundary. A DDS packs its
// rows tight, so this only works when the row is already a multiple of 256 -
// true for any DXT5 at least 64 wide, or DXT1 at least 128, which covers
// essentially every texture in the game. Anything else is left alone.
bool WriteXpr2Header(uint8_t* blob, uint32_t width, uint32_t height,
                     uint32_t format, uint32_t block) {
  if (width == 0 || height == 0 || width > 4096 || height > 4096) {
    return false;
  }

  const uint32_t blocks_w = std::max(1u, (width + 3) / 4);
  const uint32_t blocks_h = std::max(1u, (height + 3) / 4);
  const uint32_t row_bytes = blocks_w * block;
  // The pitch field holds the row in pixels >> 5, so it can only describe a
  // row that is a whole number of 32-pixel groups. A DDS packs its rows tight,
  // and they cannot be moved - there is no spare room in the blob - so a
  // width that is not a multiple of 32 would need padding this cannot do.
  if (width % 32 != 0) {
    return false;
  }

  constexpr uint32_t kRecord = 56;
  constexpr uint32_t kDesc = kRecord + 40;   // 96, ends at 120
  std::memset(blob, 0, 128);
  std::memcpy(blob, "XPR2", 4);
  StoreBE32(blob + 0x04, 128 - 12);
  StoreBE32(blob + 0x08, row_bytes * blocks_h);
  StoreBE32(blob + 0x0C, 1);
  std::memcpy(blob + 0x10, "TX2D", 4);
  StoreBE32(blob + 0x14, kRecord);
  StoreBE32(blob + 0x18, 52);
  StoreBE32(blob + 0x1C, 24);
  std::memcpy(blob + 0x24, "strName", 8);
  StoreBE32(blob + kRecord + 12, 3);
  StoreBE32(blob + kRecord + 16, 1);
  StoreBE32(blob + kRecord + 32, 0xFFFF0000u);
  StoreBE32(blob + kRecord + 36, 0xFFFF0000u);

  uint8_t* d = blob + kDesc;
  const uint32_t pitch_pixels = (row_bytes / block) * 4;
  StoreBE32(d + 0, 2u | (((pitch_pixels >> 5) & 0x1FF) << 22));   // type 2, linear
  StoreBE32(d + 4, format & 0x3F);                                 // endianness 0
  StoreBE32(d + 8, ((width - 1) & 0x1FFF) | (((height - 1) & 0x1FFF) << 13));
  StoreBE32(d + 12, 0x00000D10u);   // filtering, copied from the stock textures
  StoreBE32(d + 16, 0);             // base mip only
  // 2D, and "packed mips" set. Not optional: every texture in the stock
  // archives has that bit, and one without it fails to load.
  StoreBE32(d + 20, (1u << 9) | (1u << 11));
  return true;
}

bool WrapDdsAsXpr2(uint8_t* blob) {
  if (!(LoadLE32(blob + 80) & 0x4)) {   // DDPF_FOURCC
    return false;
  }
  const char* fourcc = reinterpret_cast<const char*>(blob + 84);
  uint32_t format = 0, block = 0;
  if (!std::memcmp(fourcc, "DXT1", 4)) {
    format = kFormatDXT1;
    block = 8;
  } else if (!std::memcmp(fourcc, "DXT2", 4) || !std::memcmp(fourcc, "DXT3", 4)) {
    format = kFormatDXT2_3;
    block = 16;
  } else if (!std::memcmp(fourcc, "DXT4", 4) || !std::memcmp(fourcc, "DXT5", 4)) {
    format = kFormatDXT4_5;
    block = 16;
  } else {
    return false;
  }
  return WriteXpr2Header(blob, LoadLE32(blob + 16), LoadLE32(blob + 12), format,
                         block);
}

// A texture straight out of a PlayStation 3 archive, which is the same thing
// in a different envelope: a 128-byte big-endian EA header over the identical
// linear DXT mip chain. Only three fields are needed, and the payload already
// begins at 128 where the XPR2 form wants it.
//
//     +0x00  u16  0x0105
//     +0x10  u32  payload offset (0x80)
//     +0x18  u8   RSX texture format: 0x86 DXT1, 0x87 DXT3, 0x88 DXT5
//     +0x20  u16  width
//     +0x22  u16  height
//
// This is what lets a PS3 mod's own `.ast` archives be used unconverted. The
// model data in them is already byte-identical to this build's - both consoles
// are big-endian PowerPC - so the textures were the only thing in the way.
bool WrapPs3AsXpr2(uint8_t* blob) {
  if (LoadBE32(blob + 0x10) != 128) {
    return false;   // pixel data is not where an XPR2 header would leave it
  }
  uint32_t format = 0, block = 0;
  switch (blob[0x18]) {
    case 0x86: format = kFormatDXT1;   block = 8;  break;
    case 0x87: format = kFormatDXT2_3; block = 16; break;
    case 0x88: format = kFormatDXT4_5; block = 16; break;
    default: return false;
  }
  return WriteXpr2Header(blob, LoadBE16(blob + 0x20), LoadBE16(blob + 0x22),
                         format, block);
}

void NbaDdsSubstitute(PPCRegister& r3) {
  const uint32_t blob_addr = r3.u32;
  if (!blob_addr) {
    return;
  }
  uint8_t* blob = GuestPtr(blob_addr);
  // Two foreign containers get an XPR2 header written over their own, in
  // place, so the guest loader will take them: a plain DDS, which is what
  // mods are made in, and the PlayStation 3 archive form, which is what a PS3
  // mod's `.ast` files are full of. Neither moves its pixel data.
  if (std::memcmp(blob, "DDS ", 4) == 0) {
    if (!WrapDdsAsXpr2(blob)) {
      // Worth a warning rather than silence: a rejected texture is a visible
      // hole in the game, and the reason is always in these three numbers.
      static int complained = 0;
      if (complained < 40) {
        ++complained;
        REXLOG_WARN("dlc_textures: cannot use a {}x{} {:.4s} DDS as-is",
                    LoadLE32(blob + 16), LoadLE32(blob + 12),
                    reinterpret_cast<const char*>(blob + 84));
      }
    } else if (TraceEnabled()) {
      REXLOG_INFO("dlc_textures: wrapped a raw DDS as XPR2");
    }
  } else if (blob[0] == 0x01 && blob[1] == 0x05 && blob[0x1A] == 2) {
    const uint32_t w = LoadBE16(blob + 0x20), h = LoadBE16(blob + 0x22);
    const uint8_t fmt = blob[0x18];
    if (!WrapPs3AsXpr2(blob)) {
      static int complained = 0;
      if (complained < 40) {
        ++complained;
        REXLOG_WARN("dlc_textures: cannot use a {}x{} PS3 texture (format 0x{:02X}) as-is",
                    w, h, fmt);
      }
      return;
    }
    if (TraceEnabled()) {
      REXLOG_INFO("dlc_textures: wrapped a {}x{} PS3 texture as XPR2", w, h);
    }
  }
  if (std::memcmp(blob, "XPR2", 4) != 0) {
    return;
  }

  const uint32_t data_offset = LoadBE32(blob + 4) + 12;
  const uint32_t data_size = LoadBE32(blob + 8);
  const uint32_t desc_offset = LoadBE32(blob + 0x14) + 40;
  if (data_offset < 0x40 || data_offset > 0x10000 || data_size == 0 ||
      desc_offset < 0x28 || desc_offset + 24 > data_offset) {
    return;
  }

  // The name is a NUL-terminated lower-case string starting at 0x24 and
  // ending before the descriptor.
  const char* name_start = reinterpret_cast<const char*>(blob + 0x24);
  const size_t name_len = ::strnlen(name_start, desc_offset - 0x24);
  const std::string name(name_start, name_len);

  char key[17];
  std::snprintf(key, sizeof(key), "%016llx",
                static_cast<unsigned long long>(
                    HashPixels(blob + data_offset, data_size)));

  if (TraceEnabled()) {
    const uint8_t* d0 = blob + desc_offset;
    const uint32_t dw2 = LoadBE32(d0 + 8);
    const uint8_t* px = blob + data_offset;
    REXLOG_INFO(
        "dlc_texture_trace: {} '{}' {}x{} fmt {} room {} dataoff {} px "
        "{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
        key, name, (dw2 & 0x1FFF) + 1, ((dw2 >> 13) & 0x1FFF) + 1,
        LoadBE32(d0 + 4) & 0x3F, data_size, data_offset,
        px[0], px[1], px[2], px[3], px[4], px[5], px[6], px[7]);
  }

  const Dds* dds = name_len ? Lookup(Lower(name)) : nullptr;
  if (!dds) {
    dds = Lookup(key);
  }
  if (!dds) {
    return;
  }

  const uint32_t blocks_w = std::max(1u, (dds->width + 3) / 4);
  const uint32_t blocks_h = std::max(1u, (dds->height + 3) / 4);
  const uint32_t row_bytes = blocks_w * dds->block;
  const uint32_t row_pitch =
      (row_bytes + kLinearRowAlignment - 1) / kLinearRowAlignment * kLinearRowAlignment;
  const uint64_t needed = uint64_t(row_pitch) * blocks_h;

  if (needed > data_size) {
    REXLOG_WARN(
        "dlc_textures: {} needs {} bytes but the original texture only has {}; skipping",
        name, needed, data_size);
    return;
  }
  if (uint64_t(row_bytes) * blocks_h > dds->data_size) {
    REXLOG_WARN("dlc_textures: {} is truncated; skipping", name);
    return;
  }

  // Pixel data, one row of blocks at a time so each row lands on its
  // 256-byte boundary. The gap between rows is left as it was; nothing
  // samples it.
  uint8_t* dest = blob + data_offset;
  for (uint32_t y = 0; y < blocks_h; ++y) {
    std::memcpy(dest + size_t(y) * row_pitch, dds->data + size_t(y) * row_bytes, row_bytes);
  }

  // Descriptor.
  uint8_t* d = blob + desc_offset;
  uint32_t dw0 = LoadBE32(d + 0);
  uint32_t dw1 = LoadBE32(d + 4);
  uint32_t dw4 = LoadBE32(d + 16);
  uint32_t dw5 = LoadBE32(d + 20);

  // Pitch is in pixels >> 5, derived from the padded row so it matches the
  // stride actually written.
  const uint32_t pitch_pixels = (row_pitch / dds->block) * 4;
  dw0 &= ~(0x1FFu << 22);
  dw0 |= ((pitch_pixels >> 5) & 0x1FF) << 22;
  dw0 &= ~(1u << 31);  // linear, not tiled

  dw1 &= ~0x3Fu;
  dw1 |= dds->format & 0x3F;
  dw1 &= ~(0x3u << 6);  // endianness: none, the DDS bytes are used as they are

  const uint32_t dw2 = ((dds->width - 1) & 0x1FFF) | (((dds->height - 1) & 0x1FFF) << 13);

  dw4 &= ~(0xFu << 2);  // mip_min_level = 0
  dw4 &= ~(0xFu << 6);  // mip_max_level = 0: base level only
  dw5 &= ~(1u << 11);   // packed_mips
  dw5 &= ~(0xFFFFFu << 12);  // mip_address

  StoreBE32(d + 0, dw0);
  StoreBE32(d + 4, dw1);
  StoreBE32(d + 8, dw2);
  StoreBE32(d + 16, dw4);
  StoreBE32(d + 20, dw5);

  REXLOG_DEBUG("dlc_textures: replaced {} with {}x{} (format {}, pitch {})", name,
               dds->width, dds->height, dds->format, pitch_pixels);
}
