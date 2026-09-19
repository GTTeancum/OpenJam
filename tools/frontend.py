#!/usr/bin/env python3
"""Put this port's main menu into a game root.

The stock main menu is built for a console that is still online: four text
entries, then a row of five icons for party sessions, leaderboards,
achievements, help and exit, and a JAMnet panel that spends its life saying it
cannot reach EA's servers. Three of those icons lead nowhere any more and the
panel never has anything to show.

What this leaves instead is six text entries - JAM NOW, ROAD TRIP, ONLINE
ARENA, HOW TO PLAY, OPTIONS, EXIT GAME - and the panel given over to the mods
that are installed, with the active one marked.

Nothing here is a new screen. Every change is made inside the game's own
files, and under the one rule that makes editing them safe: no length ever
changes. Entries are rewritten inside their own bytecode, the panel's text box
is resized by changing four numbers, and the words come from the language
database, rewritten in place. See tools/apt.py and tools/locdb.py.

Usage:
    python tools/frontend.py <game root>
    python tools/frontend.py <game root> --mods <DLC folder> --active <mod id>
"""

import argparse
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import apt
import locdb

MENU = "data/xenon/fe/bounce/screens/menus/mainmenu.ast"
PANEL = "data/xenon/fe/bounce/components/jamnet/jamfeed.ast"
STRINGS = "data/xenon/loc/{}.db"

# The panel's message field, sized and placed for a paragraph in the middle of
# an empty panel. A list wants the whole panel, from the top, ranged left.
PANEL_FIELD = "txtStatusMessage"
PANEL_BOX = {"width": 430.0, "height": 424.0, "x": 46.0, "y": 44.0,
             "align": "left"}

# A card behind each row. There is no way to add an object to a screen, so
# these are four the panel already has and no longer uses: two it hides when
# there is no leaderboard, which offline is always, and two it simply leaves
# empty. Each is pointed at the panel's own frame art - the same nine-slice
# the panel itself is drawn from - so a card looks like it belongs.
# There is no box around the active mod, and the design asks for one: orange
# around it, blue around the rest. Three things stop it, and together they
# stop it completely.
#
# A screen cannot be given new objects - that is the whole rule this port
# edits front ends under - so a box has to be something the panel already
# places and no longer uses. There are four: two the panel hides when it has
# no leaderboard, two it leaves empty. But the row highlight, the obvious
# candidate, is positioned by the list component every frame, so moving it in
# the movie does nothing; and the other three answer to coordinate spaces that
# are not the panel's, landing off the edge of it.
#
# And the part that settles it: the colour in a PlaceObject is not honoured.
# Only its alpha is. Tinting one band pure red and another pure blue left both
# exactly the grey they already were, so even a box in the right place could
# not be orange.
#
# What would work is a real display object, which means adding to the movie,
# which means moving bytes - see tools/apt_movie.py for why that does not.

# Where the mod list is written. It is the "cannot reach the servers" message,
# which is what the panel shows when it has no feed - which offline is always.
PANEL_TEXT = "TXT_JAMNET_REQUIRED_CONNECTION"

# And the footer's name for the Y button, which used to open the storefront.
# It takes BACK as well, so that a restart cannot happen by accident in a
# match - see src/mod_swap.cpp for why the runtime cannot simply tell which
# screen is up.
FOOTER_Y = ("TXT_DOWNLOAD_CONTENT", "Activate mod (+BACK)")

# And one button taken off it: Share the Shove! recommended the game to a
# friend over Xbox Live.
FOOTER_DROP = "CODE_SQUARE"

# Space for the mod list is borrowed from the online lobby's strings, which an
# offline build can never reach. See LocDb.space.
def expendable(token):
    return token.startswith("OSDK_OL")


FOOTNOTE = "BACK+Y loads the next mod. Mods live in the DLC folder."


def panel_text(mods, active):
    """The list the panel shows: the base game, then every mod installed."""
    rows = [("BASE GAME", "NBA JAM: On Fire Edition, as shipped", active is None)]
    for m in mods:
        detail = []
        if m.get("version"):
            detail.append("v%s" % m["version"])
        if m.get("author"):
            detail.append("by %s" % m["author"])
        if m.get("files"):
            detail.append("%s files" % m["files"])
        rows.append((str(m.get("name") or m.get("id") or "?").upper(),
                     " - ".join(detail), m.get("id") == active))

    out = ["MODS", ""]
    for title, detail, on in rows:
        out.append(title + ("        ACTIVE" if on else ""))
        if detail:
            out.append(detail)
        out.append("")
    out.append(FOOTNOTE)
    return "\n".join(out)


def replace(path):
    """Open a file for writing without writing through a hard link.

    An instance is built out of hard links to the base game, so writing to one
    of its files in place would edit the game itself. Removing it first breaks
    the link and leaves the original alone.
    """
    if path.exists():
        path.unlink()
    return str(path)


def apply(root, mods=(), active=None, language="eng_us"):
    """Rewrite one game root's menu, panel and text. Returns what it did."""
    root = Path(root)
    strings = root / STRINGS.format(language)
    menu, panel = root / MENU, root / PANEL
    for p in (menu, panel, strings):
        if not p.exists():
            raise SystemExit("%s: not a game root (no %s)" % (root, p.name))

    done = []

    scr = apt.Screen(str(menu))
    _log, rows = apt.rebuild_menu(scr)
    apt.drop_helpbar_entry(scr, FOOTER_DROP)
    scr.save(replace(menu))
    done.append("main menu: %d icons out, %d rows of text"
                % (len(apt.DEAD_XBOX_LIVE) + len(apt.AS_TEXT),
                   len([r for r in rows if r["array"] == 2])))

    feed = apt.Screen(str(panel))
    apt.reshape_field(feed, PANEL_FIELD, **PANEL_BOX)
    feed.save(replace(panel))
    done.append("panel: the message field given the whole panel")

    db = locdb.LocDb(str(strings))
    for token, text in apt.RETEXT + (FOOTER_Y,):
        db.set(token, text, expendable=expendable)
    text = panel_text(mods, active)
    db.set(PANEL_TEXT, text, expendable=expendable)
    db.save(replace(strings))
    done.append("text: %d strings, and a mod list of %d character(s)"
                % (len(apt.RETEXT) + 1, len(text)))
    return done


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("root", help="the game root to rewrite")
    ap.add_argument("--mods", metavar="DLC", help="a DLC folder to list")
    ap.add_argument("--active", metavar="ID", help="the mod this root has applied")
    ap.add_argument("--language", default="eng_us")
    args = ap.parse_args(argv)

    mods = []
    if args.mods:
        for m in sorted(Path(args.mods).glob("*/manifest.json")):
            mods.append(json.loads(m.read_text(encoding="utf-8")))
    for line in apply(args.root, mods, args.active, language=args.language):
        print("  " + line)
    print("  %s" % args.root)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
