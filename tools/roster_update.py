#!/usr/bin/env python3
"""Fold a bundled roster update into the archives the game already opens.

    python tools/roster_update.py <rend_roster_update_big.ast> <game data folder>

EA published a roster update for this game in 2012 - the rookies of that
year, twenty-three of them, with their faces, their bodies and their names.
It is a pair of files that sit beside the game rather than inside it, and the
mods for the PlayStation 3 version bundle their copy of it.

The Xbox 360 build will not read that copy. It wants the pair in its own
format and goes looking for a second file the PlayStation 3 one does not
have; given only half of what it expects it walks off the end of a pointer
and takes the game with it.

So rather than install the update as an update, this takes it apart and puts
its contents where the game looks for everything else: the player art into
the player archive, the front-end portraits and name plates into the
front-end one. The game then finds those players the same way it finds the
ones it shipped with, and never knows an update was involved.

This matters because a mod's rosters refer to those players. Without their
art a mod like the Legends edition has teams it cannot build - Cleveland is
one - and the game stops dead on the loading screen rather than saying so.
"""

import argparse
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ast_repack import (Archive, is_ps3_texture, is_dds, ps3_to_xpr2,  # noqa: E402
                        dds_to_xpr2, stock_pitches)
from ast_textures import ps3_to_dds  # noqa: E402

# Where each kind of entry belongs. The front end keeps its portraits and
# name plates apart from the models the match uses.
FRONT_END = ("PLYN_", "PLSH_")
NAME_PLATE = "_NAME_TEX"      # the strip of a player's name over their head
SKIP = ("ATTRIBDB_BIG",)      # the mod ships its own roster; this one is stale

FECRO = os.path.join("xenon", "bigs", "fecro_big.ast")
PLAYER = os.path.join("xenon", "bigs", "ge_player_big.ast")
HUD = os.path.join("xenon", "bigs", "ge_hud_big.ast")


def name_hash(name):
    """The 64-bit FNV-1a the archive files its entries under."""
    h = 0xCBF29CE484222325
    for c in name.encode("latin-1"):
        h = ((h ^ c) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def head_for(archive, name, like):
    """The bytes in front of an entry's record: its flags, its kind, its key.

    Everything but the key is copied from an entry already in the archive, so
    a new texture is described exactly as the textures beside it are.
    """
    head = bytearray(like["head"])
    key = struct.pack("<Q", name_hash(name))
    head[len(head) - 8:] = key
    return bytes(head)


def add(path, additions, quiet=False):
    """Put these {name: bytes} into the archive, replacing any of the same name.

    Rewrites the file in place. Returns (added, replaced).
    """
    a = Archive(path)
    by_name = {e["name"]: e for e in a.entries}
    replace = {}
    fresh = []
    for name, blob in sorted(additions.items()):
        if name in by_name:
            replace[by_name[name]["index"]] = blob
        else:
            fresh.append((name, blob))

    if not fresh:
        if replace:
            a.write(path, replace)
        return 0, len(replace)

    too_long = [n for n, _b in fresh if len(n) >= a.namesz]
    if too_long:
        raise ValueError("%s: no room for the name %r in %d bytes"
                         % (os.path.basename(path), too_long[0], a.namesz))

    # A texture already in the archive, to copy the flags and the kind from.
    like = None
    for e in a.entries:
        if len(e["head"]) == a.flag_bytes + a.info3:
            like = e
            break
    if like is None:
        raise ValueError("%s: nothing to model a new entry on" % path)

    files = a.files + len(fresh)
    rec = a.rec
    table_off = a.table_off
    align = 1 << a.offset_shift
    max_offset = ((1 << (a.offset_bytes * 8)) - 1) << a.offset_shift

    # The table grows into where the data used to start, so everything moves
    # down to the first aligned byte past the longer table.
    start = max(min(e["offset"] for e in a.entries),
                table_off + files * rec)
    start = (start + align - 1) // align * align

    records = []
    blobs = []
    cursor = start
    order = sorted(range(a.files), key=lambda i: a.entries[i]["offset"])
    placed = {}
    for i in order:
        e = a.entries[i]
        new = replace.get(i)
        if new is None:
            data = a.raw[e["offset"]:e["offset"] + e["zsize"]]
            usize, packed = e["usize"], e["packed"]
        else:
            packed_bytes = zlib.compress(new, 9)
            if len(packed_bytes) < len(new):
                data, usize, packed = packed_bytes, len(new), True
            else:
                data, usize, packed = new, len(new), False
        placed[i] = (cursor, len(data), usize, packed, bytes(e["head"]),
                     bytes(e["name_bytes"]))
        blobs.append((cursor, data))
        cursor = (cursor + len(data) + align - 1) // align * align

    added = []
    for name, blob in fresh:
        packed_bytes = zlib.compress(blob, 9)
        if len(packed_bytes) < len(blob):
            data, usize, packed = packed_bytes, len(blob), True
        else:
            data, usize, packed = blob, len(blob), False
        if cursor > max_offset:
            raise ValueError(
                "%s: the archive would pass the %d MB its offsets can reach"
                % (os.path.basename(path), max_offset // (1 << 20)))
        name_bytes = name.encode("latin-1").ljust(a.namesz, b"\0")
        added.append((cursor, len(data), usize, packed,
                      head_for(a, name, like), name_bytes))
        blobs.append((cursor, data))
        cursor = (cursor + len(data) + align - 1) // align * align

    for i in range(a.files):
        records.append(placed[i])
    records.extend(added)

    # The table is in order of key and the game binary-searches it: an entry
    # in the wrong place is an entry the game will never find, however well
    # formed it is. Python's sort is stable, so entries that share a key stay
    # in the order the archive had them.
    records.sort(key=lambda r: struct.unpack("<Q", r[4][-8:])[0])

    out = bytearray(a.raw[:start])
    # The entry count, and the size of the prefix and table together.
    struct.pack_into("<I", out, 12, files)
    struct.pack_into("<I", out, 24, a.num * 4 + files * rec)
    for n, (offset, zsize, usize, packed, head, name_bytes) in enumerate(records):
        r = bytearray(head)
        r += Archive._put(offset >> a.offset_shift, a.offset_bytes)
        r += Archive._put(zsize, a.zsize_bytes)
        r += Archive._put(usize - zsize if packed else 0, a.size_bytes)
        r += name_bytes
        assert len(r) == rec, (len(r), rec)
        p = table_off + n * rec
        out[p:p + rec] = r

    for offset, data in blobs:
        if len(out) < offset:
            out += b"\0" * (offset - len(out))
        out[offset:offset + len(data)] = data

    with open(path, "wb") as f:
        f.write(bytes(out))
    if not quiet:
        print("   %-22s %d added, %d replaced, now %.1f MB"
              % (os.path.basename(path), len(fresh), len(replace),
                 len(out) / 1048576))
    return len(fresh), len(replace)


def merge(update_path, data_root, quiet=False):
    """Take the update apart and put it where the game will find it.

    `data_root` is a game's `data` folder. Returns how many entries moved.
    """
    fecro = os.path.join(data_root, FECRO)
    player = os.path.join(data_root, PLAYER)
    hud = os.path.join(data_root, HUD)
    for p in (fecro, player, hud):
        if not os.path.isfile(p):
            raise ValueError("%s is not there - is this a game data folder?" % p)

    # The two archives do not hold textures the same way. The front end keeps
    # plain DDS, which both consoles read; the models are in the 360's own
    # format. Putting one where the other belongs gets a texture the loader
    # has no reader for, and the game calls through the hole where the reader
    # should have been. So each entry is made to match where it is going.
    pitches = stock_pitches(os.path.join(os.path.dirname(data_root), "Root",
                                         "data", PLAYER)) or {}

    a = Archive(update_path)
    front, models, plates, skipped = {}, {}, {}, 0
    for e in a.entries:
        name = e["name"]
        if name in SKIP:
            skipped += 1
            continue
        blob = a.read(e)
        to_front = name.startswith(FRONT_END)
        plate = name.endswith(NAME_PLATE)
        if to_front:
            out = blob if is_dds(blob) else (
                ps3_to_dds(blob) if is_ps3_texture(blob) else blob)
        elif is_ps3_texture(blob):
            out = ps3_to_xpr2(blob, name, pitches)
        elif is_dds(blob):
            out = dds_to_xpr2(blob, name, pitches)
        else:
            out = blob
        if out is None:
            skipped += 1
            continue
        if to_front:
            front[name] = out
        elif plate:
            plates[name] = out
        else:
            models[name] = out

    if not quiet:
        print("  roster update: %d player picture(s), %d name plate(s), "
              "%d model texture(s)" % (len(front), len(plates), len(models)))
    moved = 0
    for path, what in ((fecro, front), (hud, plates), (player, models)):
        if what:
            a_, r_ = add(path, what, quiet)
            moved += a_ + r_
    return moved


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("update", help="rend_roster_update_big.ast from the mod")
    ap.add_argument("data_root", help="the game's data folder")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)
    merge(args.update, args.data_root, args.quiet)
    return 0


if __name__ == "__main__":
    sys.exit(main())
