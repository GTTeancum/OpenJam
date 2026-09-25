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
import shutil
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

# A box around every row: orange around the mod that is loaded, blue around
# the rest, as the design has it.
#
# A screen cannot be given new objects, so a box is two objects the panel
# already places and no longer needs, pointed at the one piece of art in it
# that is a plain light tile. The first is stretched to the size of the box
# and takes the colour; the second is stretched two units smaller, sits on top
# in the panel's own near-black, and cuts the middle out. What is left is an
# outline.
#
# Four things had to be true and none of them was obvious.
#
# The panel positions and sizes these objects in its setup, for a leaderboard
# that offline never arrives, so every statement naming one is silenced first
# or the movie's placement is overwritten before the first frame.
#
# A placement only takes a colour if its flags say it has one. That is why
# half of these ignored a tint and half took it, and it is a one-bit fix -
# apt.place sets it.
#
# The colour multiplies, so it can tint art but never brighten it. The panel's
# own frame is nearly black and comes out nearly black whatever it is tinted;
# the light tile is the only art here that takes a colour and shows it.
#
# And a slot has to sit directly on the panel. The gloss overlay looks like a
# candidate and is not: it hangs off a parent that is scaled, so a box put
# there comes out wider than the panel and over its edge.
CARD_ART = 46                       # the one plain light tile the panel has
CARD_EDGE = ("mcPanelGradient", "mcUpperLine", "mcLowerLine", "mcArrow")
CARD_FILL = ("mcScrollBar", "mcItemList", "mcItemGrid", "mcHighlight")
CARD_ACTIVE = 0xFFFFA23C
CARD_IDLE = 0xFF4A86E8
CARD_INSIDE = 0xFF0B1220            # the panel's own near-black
CARD_BORDER = 2.0

# Geometry in the panel's own units, measured off the screen: a line of
# 16-point text is 20 units, a row is three of them, the first starts at 76,
# and the tile is two units wide by one and a bit tall.
CARD_TOP = 76.0
CARD_PITCH = 60.0
CARD_HEIGHT = 54.0
CARD_X = 26.0
CARD_WIDTH = 446.0
ART_W, ART_H = 2.0, 1.63

# Where the mod list is written. It is the "cannot reach the servers" message,
# which is what the panel shows when it has no feed - which offline is always.
PANEL_TEXT = "TXT_JAMNET_REQUIRED_CONNECTION"

# And the footer's name for the Y button, which used to open the storefront.
# It takes the left trigger as well, so that a restart cannot happen by
# accident in a match - see src/mod_swap.cpp for why the runtime cannot simply
# tell which screen is up, and why the second input is not Back.
FOOTER_Y = ("TXT_DOWNLOAD_CONTENT", "Choose a mod (+LT)")

# And one button taken off it: Share the Shove! recommended the game to a
# friend over Xbox Live.
FOOTER_DROP = "CODE_SQUARE"

# Space for the mod list is borrowed from the online lobby's strings, which an
# offline build can never reach. See LocDb.space.
def expendable(token):
    return token.startswith("OSDK_OL")


FOOTNOTE = "Hold LT and press Y to change mods. F9 does it too."


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


# Where a root keeps the three files this rewrites, exactly as they arrived.
#
# The rewrite is not something that can be done twice: it takes the dead Xbox
# Live entries out of the menu and puts the mod list in the panel, and run
# again it finds neither of the things it is looking for. But it does have to
# be done again - every time a mod is installed or removed, every other root's
# list is out of date - so each root keeps the originals and starts from them.
ORIGINALS = "ui.original"


def keep_original(root, path):
    """The untouched file, restored if it is already kept, saved if not."""
    stash = Path(root) / ORIGINALS / Path(path).name
    if stash.is_file():
        if path.exists():
            path.unlink()               # never write through a hard link
        shutil.copy2(stash, path)
    else:
        stash.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, stash)


def apply(root, mods=(), active=None, language="eng_us"):
    """Rewrite one game root's menu, panel and text. Returns what it did."""
    root = Path(root)
    strings = root / STRINGS.format(language)
    menu, panel = root / MENU, root / PANEL
    for p in (menu, panel, strings):
        if not p.exists():
            raise SystemExit("%s: not a game root (no %s)" % (root, p.name))
        keep_original(root, p)

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
    rows = [None] + [m.get("id") for m in mods]
    b = CARD_BORDER
    for i, (edge, fill) in enumerate(zip(CARD_EDGE, CARD_FILL)):
        apt.silence(feed, edge)
        apt.silence(feed, fill)
        if i >= len(rows):
            apt.place(feed, edge, sy=0.0)       # no row, no box
            apt.place(feed, fill, sy=0.0)
            continue
        top = CARD_TOP + CARD_PITCH * i
        apt.place(feed, edge, char=CARD_ART, x=CARD_X, y=top,
                  sx=CARD_WIDTH / ART_W, sy=CARD_HEIGHT / ART_H,
                  colour=CARD_ACTIVE if rows[i] == active else CARD_IDLE)
        apt.place(feed, fill, char=CARD_ART, x=CARD_X + b, y=top + b,
                  sx=(CARD_WIDTH - 2 * b) / ART_W,
                  sy=(CARD_HEIGHT - 2 * b) / ART_H, colour=CARD_INSIDE)
    feed.save(replace(panel))
    done.append("panel: %d box(es), orange on row %d"
                % (min(len(rows), len(CARD_EDGE)),
                   rows.index(active) + 1 if active in rows else 1))

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
