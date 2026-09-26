#!/usr/bin/env python3
"""Convert the roster update a mod bundles, so this build can load it too.

    python tools/roster_update.py <rend_roster_update_big.ast> <the mod folder>

EA published one last roster update for this game in January 2013 - the
rookies of that year, twenty-three of them, with their faces, their bodies
and their names. It arrived through the game's own Online Arena, from servers
that were switched off in 2023, so the only copies left are the ones the mods
carry: every published mod bundles it, because every published mod's rosters
name those players.

The game looks for it beside its data, by name, and asks the console's update
device for it as well. So nothing has to be taken apart or merged anywhere:
the file is converted to this build's texture format and put where the game
already looks, and the game does the rest itself.

What the conversion has to get right is that the archive holds three kinds of
thing at once, and says so in its own header: an attribute database, the
front end's portraits as plain DDS, and the match models' textures in the
PlayStation 3's format. Only the last of those wants converting. Convert the
DDS ones as well - which is what a blanket conversion does - and the front
end is handed textures it has no reader for, and the game walks off the end
of a pointer the first time it tries to draw one.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ast_repack import Archive, is_ps3_texture, ps3_to_xpr2  # noqa: E402

# What the game calls it, and where it looks: beside the data folder, in the
# root of the game, which is `D:\` to the guest.
NAME = "rend_roster_update_big.ast"


def convert(src, dst, quiet=False):
    """Write the update out in this build's format. Returns textures changed.

    Everything that is not a PlayStation 3 texture is copied through
    untouched: the attribute database, and the front end's own DDS images,
    which both consoles read the same way.
    """
    a = Archive(src)
    payloads = {}
    for e in a.entries:
        blob = a.read(e)
        if not is_ps3_texture(blob):
            continue
        out = ps3_to_xpr2(blob, e["name"])
        if out is not None:
            payloads[e["index"]] = out
    if not payloads:
        return 0
    # The archive names the kinds of resource it holds in front of its entry
    # table, and the game dispatches on that name. Converted textures are no
    # longer the PlayStation's.
    a.retag_textures()
    parent = os.path.dirname(os.path.abspath(dst))
    if parent:
        os.makedirs(parent, exist_ok=True)
    a.write(dst, payloads)
    if not quiet:
        print("  roster update: %d player texture(s) converted, %d entry(s) "
              "left as they were" % (len(payloads), a.files - len(payloads)))
    return len(payloads)


def install(update_path, mod_root, quiet=False):
    """Put the converted update in the mod, where the game will find it."""
    return convert(update_path, os.path.join(mod_root, NAME), quiet)


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("update", help="rend_roster_update_big.ast from the mod")
    ap.add_argument("mod_root",
                    help="the mod folder to put the converted one in")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)
    install(args.update, args.mod_root, args.quiet)
    return 0


if __name__ == "__main__":
    sys.exit(main())
