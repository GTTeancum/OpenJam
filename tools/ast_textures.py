#!/usr/bin/env python3
"""Read EA BGFA (.ast) archives and export their textures as DDS.

Two texture containers are involved, and neither of them is DDS on disk:

- **Xbox 360** entries are `XPR2` resources. The pixel data is tiled and
  16-bit byte swapped, with a GPU texture fetch constant in the header.
- **PlayStation 3** entries are a 128-byte EA header over a plain linear
  DXT mip chain - which is exactly what a DDS file contains, so converting
  one to DDS is a header swap and nothing more.

The PS3 header, big endian:

    0x00  u16 0x0105     version
    0x04  u32            payload size, padded
    0x10  u32            payload offset (0x80)
    0x14  u32            payload size, actual
    0x18  u8             GCM texture format: 0x86 DXT1, 0x87 DXT3, 0x88 DXT5
    0x19  u8             mip count
    0x1A  u8             dimensionality (2 = 2D)
    0x20  u16, u16       width, height

That is what makes DDS the right interchange format for mods here: the PS3
mods this port wants to load are already storing DDS payloads, and the modding
scene already works in DDS.

Usage:
    python tools/ast_textures.py list <file.ast> [name-filter]
    python tools/ast_textures.py export <file.ast> <out-dir> [name-filter]
"""

import argparse
import os
import struct
import sys
import zlib

# GCM texture format -> (DDS FourCC, bytes per 4x4 block)
GCM_FORMATS = {
    0x86: (b"DXT1", 8),
    0x87: (b"DXT3", 16),
    0x88: (b"DXT5", 16),
}


class Ast:
    """EA BGFA archive. Layout per aluigi's nbajamfire.bms; little endian."""

    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        h = self.f.read(0x30)
        if h[:4] != b"BGFA":
            raise ValueError("{}: not a BGFA archive".format(path))
        self.version = h[4:8].decode("ascii")
        (_dummy, self.files, self.info_off, _z1,
         self.info_size, _z2) = struct.unpack_from("<IIIIII", h, 8)
        (self.info1, self.info2, self.info3, self.offset_bytes,
         self.zsize_bytes, self.size_bytes, self.offset_shift,
         self.info8) = struct.unpack_from("<8B", h, 0x20)
        self.num, self.namesz = struct.unpack_from("<II", h, 0x28)
        self.entries = self._read_entries()

    @staticmethod
    def _field(buf, pos, nbytes):
        if nbytes == 0:
            return 0, pos
        if nbytes == 1:
            return buf[pos], pos + 1
        if nbytes == 2:
            return struct.unpack_from("<H", buf, pos)[0], pos + 2
        if nbytes == 3:
            return buf[pos] | (buf[pos + 1] << 8) | (buf[pos + 2] << 16), pos + 3
        return struct.unpack_from("<I", buf, pos)[0], pos + 4

    def _read_entries(self):
        self.f.seek(self.info_off)
        rec = ((1 if self.info2 == 0 else 2) + self.info3 + self.offset_bytes +
               self.zsize_bytes + self.size_bytes + self.namesz)
        buf = self.f.read(self.num * 4 + self.files * rec)
        pos = self.num * 4
        out = []
        for _ in range(self.files):
            if self.info2 == 0:
                flags, pos = buf[pos], pos + 1
            else:
                flags, pos = struct.unpack_from("<H", buf, pos)[0], pos + 2
            pos += self.info3
            offset, pos = self._field(buf, pos, self.offset_bytes)
            zsize, pos = self._field(buf, pos, self.zsize_bytes)
            size, pos = self._field(buf, pos, self.size_bytes)
            name = buf[pos:pos + self.namesz].split(b"\0")[0].decode("latin-1")
            pos += self.namesz
            out.append({"name": name, "flags": flags,
                        "offset": offset << self.offset_shift,
                        "zsize": zsize,
                        "size": (size + zsize) if size else 0})
        return out

    def read(self, entry):
        self.f.seek(entry["offset"])
        raw = self.f.read(entry["zsize"])
        if entry["size"] == 0:
            return raw
        if entry["flags"] & 2:
            raise NotImplementedError("dk2 compression not supported")
        try:
            return zlib.decompress(raw)
        except zlib.error:
            return zlib.decompressobj(-15).decompress(raw)


def xpr2_layout(blob):
    """(name, data_offset, data_size, descriptor_offset) for an XPR2 blob.

    Neither offset is where it looks like it should be. Both are taken from
    what the guest loader itself computes, and checked against 330 textures
    across the HUD, front-end and boot archives.
    """
    data_off = struct.unpack_from(">I", blob, 0x04)[0] + 12
    data_size = struct.unpack_from(">I", blob, 0x08)[0]
    desc_off = struct.unpack_from(">I", blob, 0x14)[0] + 40
    name = blob[0x24:desc_off].split(bytes([0]))[0].decode("latin-1")
    return name, data_off, data_size, desc_off


def pixel_hash(blob):
    """FNV-1a over the pixel data; matches HashPixels in src/dds_textures.cpp.

    Textures in the front-end and boot archives are all called `strName`, so
    a name is not enough to identify one. This is.
    """
    _, off, size, _ = xpr2_layout(blob)
    h = 0xCBF29CE484222325
    for b in blob[off:off + size]:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return "{:016x}".format(h)


def kind(blob):
    if blob[:4] == b"DDS ":
        return "dds"          # the PS3 front-end archives store plain DDS
    if blob[:4] == b"XPR2":
        return "xpr2"
    if len(blob) > 0x24 and blob[0] == 0x01 and blob[1] == 0x05:
        return "ps3"
    return "other"


def ps3_to_dds(blob):
    """PS3 EA texture -> (dds bytes, (w, h, fourcc, mips))."""
    data_off = struct.unpack_from(">I", blob, 0x10)[0]
    used = struct.unpack_from(">I", blob, 0x14)[0]
    fmt, mips = blob[0x18], blob[0x19]
    w, h = struct.unpack_from(">HH", blob, 0x20)
    if fmt not in GCM_FORMATS:
        raise ValueError("unsupported GCM texture format 0x{:02X}".format(fmt))
    fourcc, block = GCM_FORMATS[fmt]

    CAPS, HEIGHT, WIDTH, PIXELFORMAT = 0x1, 0x2, 0x4, 0x1000
    LINEARSIZE, MIPMAPCOUNT = 0x80000, 0x20000
    hdr = bytearray(128)
    hdr[0:4] = b"DDS "
    struct.pack_into("<I", hdr, 4, 124)
    struct.pack_into("<I", hdr, 8,
                     CAPS | HEIGHT | WIDTH | PIXELFORMAT | LINEARSIZE |
                     (MIPMAPCOUNT if mips > 1 else 0))
    struct.pack_into("<I", hdr, 12, h)
    struct.pack_into("<I", hdr, 16, w)
    struct.pack_into("<I", hdr, 20, max(1, (w + 3) // 4) * block)
    struct.pack_into("<I", hdr, 28, mips)
    struct.pack_into("<I", hdr, 76, 32)
    struct.pack_into("<I", hdr, 80, 0x4)           # DDPF_FOURCC
    hdr[84:88] = fourcc
    struct.pack_into("<I", hdr, 108, 0x1000 | (0x400000 if mips > 1 else 0))
    return bytes(hdr) + blob[data_off:data_off + used], (w, h, fourcc.decode(), mips)


def tiled_offset(x, y, width, texel_pitch):
    """Where a block really sits, the 360's own XGAddress2DTiledOffset.

    The Xenos reads a texture in tiles rather than row by row, so the blocks
    in an XPR2 payload are not in reading order. This is the address swizzle
    the hardware uses, with x and y in blocks and texel_pitch the bytes per
    block - 8 for DXT1, 16 for DXT3 and DXT5.
    """
    aligned_width = (width + 31) & ~31
    log_bpp = (texel_pitch >> 2) + ((texel_pitch >> 1) >> (texel_pitch >> 2))
    macro = ((x >> 5) + (y >> 5) * (aligned_width >> 5)) << (log_bpp + 7)
    micro = ((x & 7) + ((y & 6) << 2)) << log_bpp
    offset = (macro + ((micro & ~15) << 1) + (micro & 15) +
              ((y & 8) << (3 + log_bpp)) + ((y & 1) << 4))
    return ((((offset & ~511) << 3) + ((offset & 448) << 2) + (offset & 63) +
             ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >>
            log_bpp)


# What the fetch constant calls each compressed format, and its block size.
XPR2_FOURCC = {18: b"DXT1", 19: b"DXT3", 20: b"DXT5"}
XPR2_BLOCK = {18: 8, 19: 16, 20: 16}


def xpr2_to_dds(blob):
    """A 360 texture as a plain DDS: untiled, and byte swapped back.

    Returns (dds bytes, width, height) or raises ValueError for a format this
    does not handle. The fields are read the way tools/ast_repack.py writes
    them, which is how the stock archives have them.
    """
    _name, off, size, desc = xpr2_layout(blob)
    w1, w2 = struct.unpack_from(">2I", blob, desc + 4)
    fmt = w1 & 0x3F
    endian = (w1 >> 6) & 3
    width = (w2 & 0x1FFF) + 1
    height = ((w2 >> 13) & 0x1FFF) + 1
    if fmt not in XPR2_FOURCC:
        raise ValueError("format {} is not a DXT one".format(fmt))
    src = blob[off:off + size]
    if endian:
        src = bytes(b for pair in zip(src[1::2], src[0::2]) for b in pair)
    bpb = XPR2_BLOCK[fmt]
    bw, bh = max(1, (width + 3) // 4), max(1, (height + 3) // 4)
    out = bytearray(bw * bh * bpb)
    for by in range(bh):
        for bx in range(bw):
            at = tiled_offset(bx, by, bw, bpb) * bpb
            if at + bpb <= len(src):
                out[(by * bw + bx) * bpb:(by * bw + bx) * bpb + bpb] = \
                    src[at:at + bpb]

    hdr = bytearray(128)
    hdr[0:4] = b"DDS "
    struct.pack_into("<I", hdr, 4, 124)
    struct.pack_into("<I", hdr, 8, 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000)
    struct.pack_into("<I", hdr, 12, height)
    struct.pack_into("<I", hdr, 16, width)
    struct.pack_into("<I", hdr, 20, len(out))
    struct.pack_into("<I", hdr, 76, 32)
    struct.pack_into("<I", hdr, 80, 0x4)
    hdr[84:88] = XPR2_FOURCC[fmt]
    struct.pack_into("<I", hdr, 108, 0x1000)
    return bytes(hdr) + bytes(out), width, height


def cmd_list(args):
    a = Ast(args.archive)
    print("{}  BGFA {}  {} entries".format(
        os.path.basename(args.archive), a.version, a.files))
    kinds = {}
    shown = 0
    for e in a.entries:
        if args.filter and args.filter.lower() not in e["name"].lower():
            continue
        try:
            blob = a.read(e)
        except NotImplementedError:
            kinds["dk2"] = kinds.get("dk2", 0) + 1
            continue
        k = kind(blob)
        kinds[k] = kinds.get(k, 0) + 1
        if shown < args.limit:
            extra = ""
            if k == "ps3":
                try:
                    _, info = ps3_to_dds(blob)
                    extra = "  {}x{} {} {} mips".format(*info)
                except ValueError as exc:
                    extra = "  ({})".format(exc)
            print("  {:<44s} {:<5s} {:>10,}{}".format(e["name"], k, len(blob), extra))
        shown += 1
    print("  {} entries shown of {} matched".format(min(shown, args.limit), shown))
    print("  content: " + ", ".join("{}={}".format(k, v) for k, v in sorted(kinds.items())))
    return 0


def cmd_export(args):
    a = Ast(args.archive)
    os.makedirs(args.out, exist_ok=True)
    done = skipped = failed = 0
    for e in a.entries:
        if args.filter and args.filter.lower() not in e["name"].lower():
            continue
        try:
            blob = a.read(e)
        except NotImplementedError:
            skipped += 1
            continue
        k = kind(blob)
        if k == "dds":
            dds = blob       # already what we want
        elif k == "ps3":
            try:
                dds, _ = ps3_to_dds(blob)
            except ValueError:
                failed += 1
                continue
        else:
            skipped += 1
            continue
        safe = "".join(c if c.isalnum() or c in "._-" else "_" for c in e["name"])
        with open(os.path.join(args.out, safe + ".dds"), "wb") as fh:
            fh.write(dds)
        done += 1
    print("exported {} texture(s) to {}".format(done, args.out))
    if skipped:
        print("  {} entry(s) skipped: not a texture".format(skipped))
    if failed:
        print("  {} entry(s) failed: unsupported format".format(failed))
    return 0


def cmd_keys(args):
    """The two ways a replacement .dds can be named, for every texture here."""
    a = Ast(args.archive)
    FMT = {18: "DXT1", 19: "DXT2_3", 20: "DXT4_5"}
    shown = 0
    for e in a.entries:
        if args.filter and args.filter.lower() not in e["name"].lower():
            continue
        try:
            blob = a.read(e)
        except NotImplementedError:
            continue
        if blob[:4] != b"XPR2":
            continue
        name, _, size, desc = xpr2_layout(blob)
        d = struct.unpack_from(">6I", blob, desc)
        w = (d[2] & 0x1FFF) + 1
        h = ((d[2] >> 13) & 0x1FFF) + 1
        print("  {:<16s} {:<26s} {:>5}x{:<5} {:<7s} {:>10,}".format(
            pixel_hash(blob), name, w, h, FMT.get(d[1] & 0x3F, "?"), size))
        shown += 1
    print("  {} texture(s)".format(shown))
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("list")
    p.add_argument("archive")
    p.add_argument("filter", nargs="?")
    p.add_argument("--limit", type=int, default=25)
    p.set_defaults(func=cmd_list)

    p = sub.add_parser("keys", help="names and content hashes of 360 textures")
    p.add_argument("archive")
    p.add_argument("filter", nargs="?")
    p.set_defaults(func=cmd_keys)

    p = sub.add_parser("export", help="write every PS3 texture out as .dds")
    p.add_argument("archive")
    p.add_argument("out")
    p.add_argument("filter", nargs="?")
    p.set_defaults(func=cmd_export)

    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
