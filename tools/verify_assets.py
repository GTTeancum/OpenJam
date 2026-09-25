#!/usr/bin/env python3
"""Check that every texture in a game folder is one the game can actually use.

    python tools/verify_assets.py "the game folder"

A mod arrives as PlayStation 3 files and is converted on the way in. If a
texture comes out wrong - a size that does not match the data behind it, a
format the console never had, a header that points past the end of the file -
the game asks for it, gets nothing back, and stops. This finds those before a
player does.

Every archive in the folder is opened and every texture in it is measured
against its own header. Anything that does not add up is printed with the
archive and entry it came from. Exit code is 1 if anything is wrong, so this
can be the last step of an install.
"""

import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ast_textures import Ast, kind, xpr2_layout, XPR2_BLOCK  # noqa: E402

# The uncompressed Xenos formats this game uses, and how many bits each
# pixel takes. Anything not here and not a DXT one is a texture the game
# was never built to read.
FLAT_BITS = {2: 8, 3: 16, 4: 16, 5: 16, 6: 32, 10: 16, 16: 16, 26: 32}


def bad_xpr2(blob):
    """What is wrong with this 360 texture, or None if nothing is."""
    if len(blob) < 0x40:
        return "only %d bytes long" % len(blob)
    try:
        _name, off, size, desc = xpr2_layout(blob)
    except Exception as exc:                       # noqa: BLE001
        return "unreadable header (%s)" % exc
    if desc + 12 > len(blob):
        return "header points past the end of the file"
    if off + size > len(blob):
        return "says %d bytes of pixels but only %d are there" % (
            size, max(0, len(blob) - off))
    if size == 0:
        return "no pixels at all"
    w1, w2 = struct.unpack_from(">2I", blob, desc + 4)
    fmt = w1 & 0x3F
    width = (w2 & 0x1FFF) + 1
    height = ((w2 >> 13) & 0x1FFF) + 1
    if width > 4096 or height > 4096:
        return "claims to be %dx%d" % (width, height)
    if fmt in XPR2_BLOCK:
        bpb = XPR2_BLOCK[fmt]
        need = max(1, (width + 3) // 4) * max(1, (height + 3) // 4) * bpb
    elif fmt in FLAT_BITS:
        need = (width * height * FLAT_BITS[fmt] + 7) // 8
    else:
        return "format %d is not one the game reads" % fmt
    # More than the base level is normal: the smaller mipmaps follow it.
    if size < need:
        return "%dx%d needs %d bytes of pixels, has %d" % (
            width, height, need, size)
    return None


def bad_dds(blob):
    if len(blob) < 128:
        return "only %d bytes long" % len(blob)
    if struct.unpack_from("<I", blob, 4)[0] != 124:
        return "not a DDS header after all"
    height = struct.unpack_from("<I", blob, 12)[0]
    width = struct.unpack_from("<I", blob, 16)[0]
    if not width or not height or width > 4096 or height > 4096:
        return "claims to be %dx%d" % (width, height)
    pf = struct.unpack_from("<I", blob, 80)[0]
    four = bytes(blob[84:88])
    if pf & 0x4:                                   # DDPF_FOURCC: a DXT one
        bpb = {b"DXT1": 8, b"DXT3": 16, b"DXT5": 16}.get(four)
        if bpb is None:
            return "compressed as %r, which the game does not read" % four
        need = max(1, (width + 3) // 4) * max(1, (height + 3) // 4) * bpb
    elif pf & 0x40:                                # DDPF_RGB: plain pixels
        bits = struct.unpack_from("<I", blob, 88)[0]
        if bits not in (8, 16, 24, 32):
            return "%d bits a pixel, which the game does not read" % bits
        need = (width * height * bits + 7) // 8
    else:
        return "stored in a way the game does not read"
    if len(blob) - 128 < need:
        return "%dx%d needs %d bytes of pixels, has %d" % (
            width, height, need, len(blob) - 128)
    return None


def check_archive(path):
    """[(entry name, what is wrong)] for one archive."""
    try:
        a = Ast(path)
    except Exception as exc:                       # noqa: BLE001
        return [("", "cannot be opened (%s)" % exc)]
    out = []
    # An archive holds its textures one way throughout. The front end keeps
    # plain DDS; everything the match draws is in the console's own format.
    # A texture in the other one has no reader and takes the game with it, so
    # what the archive mostly holds is what every texture in it must be.
    kinds = {}
    blobs = {}
    for e in a.entries:
        if not e["zsize"]:
            continue
        try:
            blob = a.read(e)
        except Exception:                          # noqa: BLE001
            continue
        what = kind(blob)
        if what in ("dds", "xpr2"):
            kinds[what] = kinds.get(what, 0) + 1
        blobs[e["name"]] = what
    prevailing = None
    if len(kinds) > 1:
        prevailing = max(kinds, key=kinds.get)
        if kinds[prevailing] < 0.9 * sum(kinds.values()):
            prevailing = None                      # genuinely mixed; say nothing
    for name, what in blobs.items():
        if prevailing and what in ("dds", "xpr2") and what != prevailing:
            out.append((name, "is stored as %s in an archive of %s - the game "
                              "has no reader for it here" % (what, prevailing)))

    for e in a.entries:
        if not e["zsize"]:
            continue
        try:
            blob = a.read(e)
        except NotImplementedError:
            continue                               # compressed; not a texture
        except Exception as exc:                   # noqa: BLE001
            out.append((e["name"], "will not unpack (%s)" % exc))
            continue
        what = kind(blob)
        wrong = bad_xpr2(blob) if what == "xpr2" else (
            bad_dds(blob) if what == "dds" else None)
        if what == "ps3":
            wrong = "is still a PlayStation 3 texture - it was never converted"
        if wrong:
            out.append((e["name"], wrong))
    return out


def verify(root, quiet=False):
    """Returns the number of broken textures found under root."""
    archives = []
    for base, _dirs, files in os.walk(root):
        for f in files:
            if f.lower().endswith(".ast"):
                archives.append(os.path.join(base, f))
    archives.sort()
    broken = 0
    for path in archives:
        problems = check_archive(path)
        if problems:
            rel = os.path.relpath(path, root)
            for name, wrong in problems:
                broken += 1
                print("   %s: %s %s" % (rel, name or "(archive)", wrong))
    if not quiet:
        print("checked %d archive(s): %s" % (
            len(archives),
            "all textures are sound" if not broken
            else "%d bad texture(s)" % broken))
    return broken


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("root", help="a game folder, or a mod's data folder")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)
    return 1 if verify(args.root, args.quiet) else 0


if __name__ == "__main__":
    sys.exit(main())
