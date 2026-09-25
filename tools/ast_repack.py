#!/usr/bin/env python3
"""Rewrite a PlayStation 3 `.ast` archive so this Xbox 360 build can read it.

Why this is only about textures
-------------------------------

The two console builds of NBA JAM: On Fire Edition ship the same archives with
the same names. Comparing them entry by entry:

    ge_player_big.ast       518 non-texture entries, 515 byte-identical
    ge_environment_big.ast  802 non-texture entries, 802 byte-identical
    ge_ball_big.ast          23 non-texture entries,  23 byte-identical
    ge_hud_big.ast          248 non-texture entries, 248 byte-identical

Models, meshes, skeletons and animation are literally the same bytes - both
consoles are big-endian PowerPC, and EA shipped one set of geometry. The only
thing that differs is the texture container, and that is the only reason a PS3
mod's archives cannot be dropped straight into this build.

So this converts the textures and copies everything else through untouched.

The two containers
------------------

A PS3 texture is a 128-byte big-endian EA header over a plain linear DXT mip
chain:

    +0x00  u16  version, 0x0105 or 0x0201
    +0x10  u32  payload offset (0x80)
    +0x14  u32  payload size
    +0x18  u8   RSX format: 0x86 DXT1, 0x87 DXT3, 0x88 DXT5
    +0x19  u8   mip count
    +0x1A  u8   2 for a 2D texture
    +0x20  u16  width
    +0x22  u16  height

A 360 texture is an XPR2 resource whose header ends in a GPU texture fetch
constant. The pixel data it points at is normally tiled and byte swapped, but
the fetch constant can also describe a plain linear texture with no swapping -
which is exactly what the PS3 payload already is. So the conversion is a new
header over the same blocks, with three fields doing the work:

    tiled      cleared
    endianness 0
    pitch      the row stride, in pixels >> 5

The pitch is not the width. Measured across 735 stock front-end textures,
every one declares its width rounded up to a multiple of 128 texels and never
less than 128 - 32 wide says 128, 144 says 256, 272 and 360 both say 384. The
hardware reads rows at that stride whatever the header claims, so the rows are
laid out to match. Being able to pad them is the advantage of doing this
offline: the runtime path in src/dds_textures.cpp rewrites a header in place
with no room to move anything.

Only the base mip level is kept. A linear mip chain has its own packing rules
on this hardware, and a texture that does not minify is a much smaller problem
than one that does not appear.

The third container
-------------------

A mod also brings plain DDS - johnz1's Legends ships 4307 of them, the whole
front-end atlas set - and so, it turns out, does the stock 360 game. Those are
converted too, with one exception: see dds_is_the_games_own.

Usage:
    python tools/ast_repack.py <in.ast> <out.ast>
    python tools/ast_repack.py --tree <dir>          convert in place
"""

import argparse
import os
import struct
import sys
import zlib

# RSX texture format -> (XPR2 format code, bytes per unit, unit size in texels,
# endianness). The endianness field says how the GPU should reorder bytes as it
# reads: DXT blocks coming from a PS3 archive need no reordering, while a
# 32-bit pixel does, exactly as the stock 360 archives ask for.
#
# 0xA5 is 0x85 (A8R8G8B8) with the RSX "normalized coordinates" flag set; the
# pixels are the same either way.
#
# The last field is the fetch constant's swizzle word, copied from whatever the
# stock archives use for that format: DXT samples straight through as RGBA,
# while a 32-bit pixel is stored BGRA and has to be swizzled back.
RSX_FORMATS = {
    0x86: (18, 8, 4, 0, 0x00000D10),    # DXT1
    0x87: (19, 16, 4, 0, 0x00000D10),   # DXT3
    0x88: (20, 16, 4, 0, 0x00000D10),   # DXT5
    0x85: (6, 4, 1, 2, 0x00000C14),     # A8R8G8B8
    0xA5: (6, 4, 1, 2, 0x00000C14),
}

# A PS3 archive pads its uncompressed levels out to this; compressed ones are
# stored tight.
LINEAR_ROW_ALIGN = 256


class Archive:
    """EA BGFA archive, read and write. Layout per aluigi's nbajamfire.bms.

    The header is little-endian even though everything inside is big-endian,
    which is what you get when the tool that wrote it ran on a PC.
    """

    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            self.raw = f.read()
        h = self.raw[:0x30]
        if h[:4] != b"BGFA":
            raise ValueError("{}: not a BGFA archive".format(path))
        self.version = h[4:8]
        (_d, self.files, self.info_off, _z1,
         self.info_size, _z2) = struct.unpack_from("<IIIIII", h, 8)
        (self.info1, self.info2, self.info3, self.offset_bytes,
         self.zsize_bytes, self.size_bytes, self.offset_shift,
         self.info8) = struct.unpack_from("<8B", h, 0x20)
        self.num, self.namesz = struct.unpack_from("<II", h, 0x28)
        self.flag_bytes = 1 if self.info2 == 0 else 2
        self.rec = (self.flag_bytes + self.info3 + self.offset_bytes +
                    self.zsize_bytes + self.size_bytes + self.namesz)
        self.prefix_off = self.info_off
        self.table_off = self.info_off + self.num * 4
        self.entries = self._read_entries()

    @staticmethod
    def _get(buf, pos, n):
        if n == 0:
            return 0
        return int.from_bytes(buf[pos:pos + n], "little")

    @staticmethod
    def _put(value, n):
        if n == 0:
            return b""
        return int(value).to_bytes(n, "little")

    def _read_entries(self):
        out = []
        for i in range(self.files):
            p = self.table_off + i * self.rec
            rec = self.raw[p:p + self.rec]
            q = self.flag_bytes + self.info3
            offset = self._get(rec, q, self.offset_bytes)
            q += self.offset_bytes
            zsize = self._get(rec, q, self.zsize_bytes)
            q += self.zsize_bytes
            size = self._get(rec, q, self.size_bytes)
            q += self.size_bytes
            name = rec[q:q + self.namesz].split(b"\0")[0].decode("latin-1")
            out.append({
                "index": i,
                "head": rec[:self.flag_bytes + self.info3],
                "name": name,
                "name_bytes": rec[q:q + self.namesz],
                "offset": offset << self.offset_shift,
                "zsize": zsize,
                # The stored field is the amount the entry grows when it is
                # unpacked, not the unpacked size; 0 means stored, not packed.
                "usize": (size + zsize) if size else zsize,
                "packed": size != 0,
            })
        return out

    def retag_textures(self):
        """Say `.XPR` where the PS3 archive said `.GTF`.

        In front of the entry table sit `num` four-byte tags naming the kinds
        of resource the archive holds, stored back to front. The texture tag is
        the only one that differs between the two consoles, and it is what the
        game dispatches on: with `.GTF` in the header it never even tries the
        XPR2 loader, hands the model a null texture, and dies on the next
        dereference. Converting the textures without this changes nothing.
        """
        old, new = b".GTF"[::-1], b".XPR"[::-1]
        pre = self.raw[self.prefix_off:self.table_off]
        if old not in pre:
            return False
        self.raw = (self.raw[:self.prefix_off] + pre.replace(old, new) +
                    self.raw[self.table_off:])
        return True

    def read(self, entry):
        blob = self.raw[entry["offset"]:entry["offset"] + entry["zsize"]]
        if not entry["packed"]:
            return blob
        try:
            return zlib.decompress(blob)
        except zlib.error:
            return zlib.decompressobj(-15).decompress(blob)

    def write(self, path, payloads):
        """payloads: index -> replacement uncompressed bytes (or None to keep)."""
        align = 1 << self.offset_shift
        max_offset = ((1 << (self.offset_bytes * 8)) - 1) << self.offset_shift

        # Keep the original file order so related entries stay near each other.
        order = sorted(range(self.files), key=lambda i: self.entries[i]["offset"])
        start = min(e["offset"] for e in self.entries)

        blobs = {}
        cursor = start
        placed = {}
        for i in order:
            e = self.entries[i]
            new = payloads.get(i)
            if new is None:
                data = self.raw[e["offset"]:e["offset"] + e["zsize"]]
                usize, packed = e["usize"], e["packed"]
            else:
                packed_bytes = zlib.compress(new, 9)
                if len(packed_bytes) < len(new):
                    data, usize, packed = packed_bytes, len(new), True
                else:
                    data, usize, packed = new, len(new), False
            if cursor > max_offset:
                raise ValueError(
                    "{}: repacked archive passes the {} MB the 3-byte offset "
                    "field can address".format(path, max_offset // (1 << 20)))
            placed[i] = (cursor, len(data), usize, packed)
            blobs[i] = data
            cursor += len(data)
            cursor = (cursor + align - 1) // align * align

        out = bytearray(self.raw[:start])
        for i in range(self.files):
            e = self.entries[i]
            offset, zsize, usize, packed = placed[i]
            assert offset % align == 0
            rec = bytearray(e["head"])
            rec += self._put(offset >> self.offset_shift, self.offset_bytes)
            rec += self._put(zsize, self.zsize_bytes)
            rec += self._put(usize - zsize if packed else 0, self.size_bytes)
            rec += e["name_bytes"]
            assert len(rec) == self.rec
            p = self.table_off + i * self.rec
            out[p:p + self.rec] = rec

        for i in order:
            offset, zsize, _u, _p = placed[i]
            if len(out) < offset:
                out += b"\0" * (offset - len(out))
            out[offset:offset + zsize] = blobs[i]

        # Nothing in the header describes the file's length, so nothing in it
        # needs updating: the entry count, the entry table's position and its
        # size are all unchanged, and only the payloads move.
        with open(path, "wb") as f:
            f.write(bytes(out))
        return len(out)


def is_ps3_texture(b):
    """The version word varies (0x0105, 0x0201); the shape does not."""
    if len(b) < 0x80 or b[0x1A] != 2 or b[0x18] not in RSX_FORMATS:
        return False
    if struct.unpack_from(">I", b, 0x10)[0] != 0x80:
        return False
    w, h = struct.unpack_from(">HH", b, 0x20)
    return 0 < w <= 4096 and 0 < h <= 4096


# A DDS is the other container a mod can arrive in - johnz1's Legends ships
# 4307 of them, mostly the front end's atlases - and its blocks need no
# reordering either, exactly as src/dds_textures.cpp found for the DDS files a
# mod drops in loose. Asking for the 8-in-16 swap the stock archives use turns
# every one of them into colour noise.
#
# Doing it here rather than at run time is what fixes the small ones. The
# runtime rewrites a header in place with nowhere to put padding, so it has to
# refuse any texture whose rows do not already start on a 256-byte boundary -
# which a 32x32 DXT5 glyph does not, and those were the scrambled ones.
#
# fourCC -> (XPR2 format code, bytes per unit, unit size in texels, endianness,
# swizzle).
DDS_FORMATS = {
    b"DXT1": (18, 8, 4, 0, 0x00000D10),
    b"DXT3": (19, 16, 4, 0, 0x00000D10),
    b"DXT5": (20, 16, 4, 0, 0x00000D10),
}
DDS_UNCOMPRESSED = (6, 4, 1, 2, 0x00000C14)


def is_dds(b):
    return (len(b) > 0x80 and b[:4] == b"DDS " and
            struct.unpack_from("<I", b, 4)[0] == 124)


def dds_to_xpr2(b, _name, pitches=None):
    """A DDS texture, whose blocks are in PC order.

    Named the way the game names its own: every texture in a front-end archive
    is called strName, the stock ones included, and strName is what the
    runtime writes when it wraps one of these in place.
    """
    h, w = struct.unpack_from("<II", b, 0x0C)
    fourcc = bytes(b[0x54:0x58])
    if fourcc in DDS_FORMATS:
        fmt, unit, texels, endian, swizzle = DDS_FORMATS[fourcc]
    elif struct.unpack_from("<I", b, 0x58)[0] == 32:
        fmt, unit, texels, endian, swizzle = DDS_UNCOMPRESSED
    else:
        return None
    if not (0 < w <= 4096 and 0 < h <= 4096):
        return None
    row = max(1, (w + texels - 1) // texels) * unit
    return build_xpr2("strName", w, h, fmt, unit, texels, endian, swizzle,
                      b[0x80:], row, (pitches or {}).get(("strname", w, h, fmt)))


# Every XPR2 texture in the stock archives carries the same 28 bytes in front
# of its fetch constant, whatever the texture is. Taken from the 360 build's
# own ge_ball_big.ast rather than invented.
XPR2_RECORD_PREFIX = bytes.fromhex(
    "00000003" "00000001" "00000000" "00000000" "00000000"
    "ffff0000" "ffff0000")


def build_xpr2(name, w, h, fmt, unit, texels, endian, swizzle, src, src_pitch,
               pitch_texels=None):
    """An XPR2 resource with linear, untiled pixel data.

    A 360 texture is an XPR2 resource whose header ends in a GPU texture fetch
    constant. The pixel data it points at is normally tiled, but the fetch
    constant can also describe a plain linear surface - which is what both a
    PS3 payload and a DDS already are, so the conversion is a new header over
    the same blocks with the tiling bit cleared and the row stride declared.

    The name matters. A stock texture stores its own name, lower case, at
    +0x24, and that is what binds a model's material to it - a texture written
    out under the wrong name loads as nothing and the model dereferences null.
    """
    rows = max(1, (h + texels - 1) // texels)
    row = max(1, (w + texels - 1) // texels) * unit
    # The row stride is not the width, and it is not always the same rule.
    #
    # Measured across 735 stock front-end textures, every one declares a pitch
    # of its width rounded up to a multiple of 128 texels and never less than
    # 128: 32 wide says 128, 144 says 256, 272 and 360 both say 384. The
    # hardware reads rows at that stride whatever the header claims, which is
    # why a 32x32 glyph padded to any smaller figure comes out torn, with
    # slices of itself below where they belong.
    #
    # Those were all compressed, and on an uncompressed texture the disc says
    # something else: the five 64x64 body maps in ge_player_big.ast declare 64,
    # not 128. Giving them 128 doubles every row's stride and the player comes
    # out in rainbow stripes from the neck down - jersey, shorts, arms and
    # legs, while the face, which is a compressed texture, stays perfect.
    #
    # So where the disc ships the same texture, its own figure is used, and
    # the measured rule is only the fallback for textures the disc has never
    # seen. See stock_pitches().
    if pitch_texels is None:
        pitch_texels = max(128, (w + 127) // 128 * 128)
    pitch = (pitch_texels // texels) * unit
    if len(src) < src_pitch * (rows - 1) + row:
        return None
    pixels = bytearray(pitch * rows)
    for y in range(rows):
        pixels[y * pitch:y * pitch + row] = src[y * src_pitch:y * src_pitch + row]

    # Layout, mirroring what the stock archives do:
    #
    #   0x24        name, NUL terminated
    #   12 + R      the 52-byte resource record, R normally 48
    #   record+28   the GPU texture fetch constant
    #   E + 12      pixel data, with E the value at +0x04
    #
    # R, the value at +0x14, is always a multiple of 16 - the records sit on a
    # 16-byte grid measured from blob+12 - and never less than 48. A name long
    # enough to push the record past 48 rounds it up to the next multiple, so
    # `onfire_ball_diffuse_dif_tex` gets R=64 where `american_ball_dif_tex`
    # gets R=48.
    label = name.lower().encode("latin-1", "replace")
    needed = 0x24 + len(label) + 1 - 12
    record = 12 + max(48, (needed + 15) // 16 * 16)
    desc = record + 28
    end = record + 52
    hdr = bytearray(end + 12)
    hdr[0:4] = b"XPR2"
    struct.pack_into(">I", hdr, 0x04, end)
    struct.pack_into(">I", hdr, 0x08, len(pixels))
    struct.pack_into(">I", hdr, 0x0C, 1)
    hdr[0x10:0x14] = b"TX2D"
    struct.pack_into(">I", hdr, 0x14, record - 12)
    struct.pack_into(">I", hdr, 0x18, 52)
    struct.pack_into(">I", hdr, 0x1C, 24)
    hdr[0x24:0x24 + len(label)] = label
    hdr[record:record + 28] = XPR2_RECORD_PREFIX

    struct.pack_into(">I", hdr, desc + 0, 2 | ((pitch_texels >> 5) & 0x1FF) << 22)
    struct.pack_into(">I", hdr, desc + 4, (fmt & 0x3F) | ((endian & 3) << 6))
    struct.pack_into(">I", hdr, desc + 8,
                     ((w - 1) & 0x1FFF) | (((h - 1) & 0x1FFF) << 13))
    struct.pack_into(">I", hdr, desc + 12, swizzle)
    struct.pack_into(">I", hdr, desc + 16, 0)      # base mip only
    # 2D, and "packed mips" set. That bit is not optional: every texture in
    # the stock archives has it, and a texture without it fails to load and
    # leaves the model holding a null pointer. It cost an afternoon.
    struct.pack_into(">I", hdr, desc + 20, (1 << 9) | (1 << 11))
    return bytes(hdr) + bytes(pixels)


def stock_pitches(path):
    """What the disc declares as the row stride for each texture it ships.

    Keyed by name and shape together: front-end archives call every texture
    `strName`, so a name alone identifies nothing there, and a wrong match
    would be worse than no match.
    """
    out = {}
    try:
        a = Archive(path)
    except Exception:
        return out
    for e in a.entries:
        try:
            blob = a.read(e)
        except Exception:
            continue
        if blob[:4] != b"XPR2":
            continue
        try:
            desc = struct.unpack_from(">I", blob, 0x14)[0] + 40
            w0, w1, w2 = struct.unpack_from(">3I", blob, desc)
        except Exception:
            continue
        name = blob[0x24:].split(bytes([0]))[0].decode("latin-1")
        key = (name, (w2 & 0x1FFF) + 1, ((w2 >> 13) & 0x1FFF) + 1, w1 & 0x3F)
        pitch = ((w0 >> 22) & 0x1FF) * 32
        # A name that turns up twice with different answers is no use.
        if key in out and out[key] != pitch:
            out[key] = None
        else:
            out[key] = pitch
    return out


def ps3_to_xpr2(b, name, pitches=None):
    """A PS3 texture, whose blocks are already in the order the 360 GPU reads."""
    fmt, unit, texels, endian, swizzle = RSX_FORMATS[b[0x18]]
    w, h = struct.unpack_from(">HH", b, 0x20)
    row = max(1, (w + texels - 1) // texels) * unit
    # Compressed levels are stored tight in a PS3 archive; uncompressed ones
    # already have their rows padded out to 256 bytes. Measured, not assumed:
    # across ge_player_big.ast all 1210 DXT textures match the tight layout and
    # all 5 of the 32-bit ones match the padded layout.
    src_pitch = ((row + LINEAR_ROW_ALIGN - 1) // LINEAR_ROW_ALIGN *
                 LINEAR_ROW_ALIGN) if texels == 1 else row
    return build_xpr2(name, w, h, fmt, unit, texels, endian, swizzle,
                      b[0x80:], src_pitch,
                      (pitches or {}).get((name.lower(), w, h, fmt)))


def holds_dds(path):
    """Whether this archive stores its textures as plain DDS."""
    try:
        a = Archive(path)
    except Exception:
        return False
    for e in a.entries:
        try:
            if a.read(e)[:4] == b"DDS ":
                return True
        except Exception:
            continue
    return False


def dds_is_the_games_own(path, stock=None):
    """Whether the DDS in this archive is the disc's business rather than ours.

    DDS is not a mod's invention: the stock 360 game ships thousands of them in
    its own archives, and src/dds_textures.cpp wraps one in an XPR2 header in
    place as it is loaded. That in-place wrap has nowhere to put padding and
    declares the row stride as the texture's own width, which the hardware does
    not honour below 128 texels - so anything narrower comes out torn, with
    slices of itself below where it belongs. Converting those here, where the
    rows can be laid out properly, is what fixes them.

    Some archives are the exception, and the disc says which. Where the stock
    360 game keeps plain DDS in an archive, whatever reads that archive wants
    plain DDS and nothing else: converting the front-end packs under bigs/
    takes the game down on team select, and converting genbigs/ - the jersey
    numbers - takes it down as a match loads, both reading through a null
    texture. So the question is put to the disc's own copy of the same file
    rather than guessed from where it sits.

    With no stock copy to ask - a file the mod adds outright - the old rule
    stands, which is that bigs/ is left alone.
    """
    if stock is not None and os.path.isfile(str(stock)):
        return holds_dds(str(stock))
    return "bigs" in str(path).replace("\\", "/").split("/")


def repack(src, dst, stock=None, quiet=False):
    a = Archive(src)
    convert_dds = not dds_is_the_games_own(src, stock)
    pitches = stock_pitches(str(stock)) if stock is not None else {}
    payloads = {}
    converted = kept = failed = 0
    for e in a.entries:
        blob = a.read(e)
        if is_ps3_texture(blob):
            out = ps3_to_xpr2(blob, e["name"], pitches)
        elif is_dds(blob) and convert_dds:
            out = dds_to_xpr2(blob, e["name"], pitches)
        else:
            kept += 1
            continue
        if out is None:
            failed += 1
            continue
        payloads[e["index"]] = out
        converted += 1
    if not converted:
        if not quiet:
            print("  {}: no PS3 textures, left alone".format(os.path.basename(src)))
        return False
    original = len(a.raw)
    retagged = a.retag_textures()
    size = a.write(dst, payloads)
    if not quiet:
        print("  {}: {} texture(s) converted{}, {} entry(s) copied{}, "
              "{:,} -> {:,} bytes"
              .format(os.path.basename(src), converted,
                      " and retagged .GTF -> .XPR" if retagged else "", kept,
                      ", {} failed".format(failed) if failed else "",
                      original, size))
    return True


def repack_tree(root):
    n = 0
    for dp, _, fns in os.walk(root):
        for fn in fns:
            if not fn.lower().endswith(".ast"):
                continue
            p = os.path.join(dp, fn)
            try:
                if not Archive(p).entries:
                    continue
            except ValueError:
                continue
            tmp = p + ".repack"
            try:
                if repack(p, tmp):
                    os.replace(tmp, p)
                    n += 1
                elif os.path.exists(tmp):
                    os.remove(tmp)
            except Exception as exc:
                if os.path.exists(tmp):
                    os.remove(tmp)
                print("  {}: {}".format(fn, exc))
    print("{} archive(s) converted".format(n))
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source")
    ap.add_argument("dest", nargs="?")
    ap.add_argument("--tree", action="store_true",
                    help="walk a directory and convert every archive in place")
    args = ap.parse_args(argv)
    if args.tree:
        return repack_tree(args.source)
    if not args.dest:
        ap.error("a destination is required unless --tree is given")
    repack(args.source, args.dest)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
