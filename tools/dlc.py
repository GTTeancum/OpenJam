#!/usr/bin/env python3
"""Mod support for the port: a DLC folder, one subfolder per mod, a manifest
each, and an instanced game root built from them.

This is deliberately not the Xbox 360's downloadable content mechanism. Mods
live in a plain folder the user can open:

    <game parent>/DLC/
        legends-on-fire-edition/
            manifest.json
            data/common/...
            data/xenon/...

`import` turns an extracted PS3 mod into one of those folders. Use
tools/ps3_pkg_extract.py first to get the tree out of the .pkg the mod ships
as.

**What transfers and what does not.** Proven by running it:

- Databases (`bounce.db`, `eng_us.db`) and XML work **directly**. With only
  those applied, the 360 build boots to a title screen reading "v2.0 by
  johnz1" - the mod's own strings, straight out of the PS3 localisation
  database.
- Texture *archives* do **not**. Dropping the mod's `.ast` files in leaves the
  menus black: this build stores XPR2 resources and the PS3 ones are not that,
  so the game fails the load quietly rather than drawing garbage.

  Textures are instead replaced individually as DDS, which the runtime reads
  directly - see `src/dds_textures.cpp`. Put `.dds` files in
  `<game root>/dlc_textures/`, named after the texture they replace.

The two data trees line up almost exactly, `data/ps3/...` against
`data/xenon/...`, with 170 files byte-identical between a modded PS3 build and
the stock 360 build. Both consoles are big-endian PowerPC, which is why so much
transfers untouched. What differs is the mod's own content plus a handful of
platform files (PS3 `.gtf` boot textures against 360 `.xpr`, PS3 save icons),
which import leaves behind.

`instance` builds a complete game root for a chosen mod: every base game file,
with the mod's files in place of the ones it replaces. It uses hard links, so
an 850 MB mod instance costs kilobytes and takes seconds rather than copying.
Point `--game_data_root` at the result. It also puts this port's main menu in
- see tools/frontend.py - so the instance lists the mods it was built from.

Usage:
    python tools/dlc.py list
    python tools/dlc.py import <extracted-ps3-USRDIR> --name "..." --version 2.0
    python tools/dlc.py instance <mod-id>
    python tools/dlc.py instance <mod-id> --include "data/xenon/database/*"
    python tools/dlc.py instance <mod-id> --exclude "*.ast"
"""

import argparse
import fnmatch
import json
import os
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ast_repack
import ast_textures
import frontend

# The port lives in <parent>/port; the game data and DLC live beside it.
PORT = Path(__file__).resolve().parent.parent
PARENT = PORT.parent
GAME_ROOT = PARENT / "Root"
DLC_ROOT = PARENT / "DLC"
INSTANCE_ROOT = PARENT / "instances"

# A game root is meant to look like an installed game: default.xex, the data
# beside it. Saves are kept together in one `saves` folder at the top of the
# staged game, a subfolder per mod, rather than inside each mod - so removing
# or rebuilding a mod cannot take anyone's progress with it.
#
# Each root carries a note saying which folder is its own, which
# src/mod_saves.cpp reads. A mod added later needs no entry anywhere: its root
# is built with the note in it and its saves follow. The note is written
# relative to the root, so the whole game folder can be moved or copied and
# the saves travel with it.
#
# Rebuilding a root therefore has to step around that folder. It is the one
# thing in there that is not rebuildable: everything else is a hard link to
# the base game or to the mod.
SAVE_DIR = "saves"
SAVE_MARKER = "saves.path"

SCHEMA = 1

# PS3-only files with no meaning on this build. `.gtf` is the PS3 texture
# container, where the 360 uses `.xpr`; saveload icons are PS3 save metadata.
#
# The two archives are the only ones whose contents really are platform code
# rather than platform-neutral data, and each was found by watching the game
# die with one applied and live with it withheld:
#
#   rend_misc_big.ast   holds "XFNR" render resources where the 360 build has
#                       its own; applying the PS3 copy faults during boot.
#   audio/aemsdata.ast  is the audio engine's bank data, and the two consoles
#                       do not share a sound format; applying it faults as a
#                       match loads.
#
# Everything else transfers, the textures after conversion (see
# tools/ast_repack.py) and the rest untouched.
SKIP_SUFFIXES = (".gtf",)
SKIP_PATTERNS = (
    re.compile(r"^xenon/saveload/"),
    re.compile(r"^xenon/genbigs/rend_misc_big\.ast$"),
    re.compile(r"^xenon/bigs/audio/aemsdata\.ast$"),
)


def slugify(text):
    s = re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")
    return s or "mod"


def rel_files(root):
    for dp, _, fns in os.walk(root):
        for fn in fns:
            p = Path(dp) / fn
            yield p, p.relative_to(root).as_posix()


def map_path(rel):
    """PS3 data path -> this build's data path."""
    return rel.replace("ps3/", "xenon/", 1) if rel.startswith("ps3/") else rel


def repack_archive(src, dst, stock=None):
    """True when src was a PS3 archive and dst now holds the converted form.

    `stock` is the disc's own copy of the same file, which is what decides
    whether the textures inside want converting at all - see
    ast_repack.dds_is_the_games_own.
    """
    try:
        return ast_repack.repack(str(src), str(dst), stock=stock, quiet=True)
    except Exception:
        if Path(dst).exists():
            Path(dst).unlink()
        return False


def unpack_pkg(pkg):
    """A PS3 package, opened out into a folder beside it.

    Mods arrive as .pkg because that is what a PS3 installs, and inside is an
    ordinary USRDIR tree. Unpacked once and kept: the same package imported
    twice does not need opening twice, and having the tree around is what
    makes a later re-import - after a fix to the converter, say - a second
    rather than a minute.
    """
    out = pkg.parent / (pkg.stem + "_ps3")
    usrdir = out / "USRDIR"
    if usrdir.is_dir() and any(usrdir.iterdir()):
        print("using the copy already opened at {}".format(out))
        return usrdir
    print("opening {}".format(pkg.name))
    result = subprocess.run(
        [sys.executable, str(PORT / "tools" / "ps3_pkg_extract.py"),
         str(pkg), str(out)],
        capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stdout.strip() or result.stderr.strip())
        return None
    for line in result.stdout.strip().splitlines()[-2:]:
        print("  " + line)
    return usrdir if usrdir.is_dir() else out


def cmd_import(args):
    src = Path(args.source).resolve()
    if src.is_file() and src.suffix.lower() == ".pkg":
        opened = unpack_pkg(src)
        if opened is None:
            print("could not open that package")
            return 1
        src = opened
    data = src / "data" if (src / "data").is_dir() else src
    if not data.is_dir():
        print("no data/ folder under {}".format(src))
        return 1

    name = args.name or src.parent.name
    mod_id = args.id or slugify(name)
    dest = DLC_ROOT / mod_id
    if dest.exists() and not args.force:
        print("{} already exists; pass --force to replace it".format(dest))
        return 1
    if dest.exists():
        shutil.rmtree(dest)

    copied = skipped = repacked = 0
    for src_path, rel in rel_files(data):
        mapped = map_path(rel)
        if mapped.endswith(SKIP_SUFFIXES) or any(p.match(mapped) for p in SKIP_PATTERNS):
            skipped += 1
            continue
        target = dest / "data" / mapped
        target.parent.mkdir(parents=True, exist_ok=True)
        # An `.ast` full of PS3 textures gets them rewritten as XPR2 on the way
        # in. Everything else in those archives - models, meshes, skeletons,
        # animation - is already byte-identical between the two consoles, so
        # this is the whole of what porting a mod's art amounts to.
        if not (mapped.lower().endswith(".ast") and
                repack_archive(src_path, target, GAME_ROOT / "data" / mapped)):
            shutil.copy2(src_path, target)
        else:
            repacked += 1
        copied += 1

    manifest = {
        "schema": SCHEMA,
        "id": mod_id,
        "name": name,
        "version": args.version or "",
        "author": args.author or "",
        "mode": "overwrite",
        "enabled": True,
        "source": {"platform": "ps3", "imported_from": str(src)},
        "files": copied,
        "notes": args.notes or "",
    }
    (dest / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    print("imported '{}' as {}".format(name, mod_id))
    print("  {} files, {} skipped as PS3-only".format(copied, skipped))
    if repacked:
        print("  {} archive(s) had their textures converted to this build's "
              "format".format(repacked))
    print("  {}".format(dest))
    return 0


def load_mods():
    mods = []
    if not DLC_ROOT.is_dir():
        return mods
    for entry in sorted(DLC_ROOT.iterdir()):
        mf = entry / "manifest.json"
        if entry.is_dir() and mf.is_file():
            try:
                mods.append((entry, json.loads(mf.read_text(encoding="utf-8"))))
            except json.JSONDecodeError as e:
                print("  ! {}: bad manifest ({})".format(entry.name, e))
    return mods


def cmd_list(args):
    mods = load_mods()
    if not mods:
        print("no mods in {}".format(DLC_ROOT))
        return 0
    print("{} mod(s) in {}\n".format(len(mods), DLC_ROOT))
    for path, m in mods:
        print("  {:34s} {:>6}  {} file(s){}".format(
            m.get("id", path.name), m.get("version", "?"), m.get("files", "?"),
            "" if m.get("enabled", True) else "  [disabled]"))
        if m.get("name"):
            print("      {}".format(m["name"]))
    return 0


def link_or_copy(src, dst):
    """Hard link when possible; the fallback matters across volumes."""
    try:
        os.link(src, dst)
    except OSError:
        shutil.copy2(src, dst)


# The base game is a choice like any other: an instance of it still wants this
# port's menu, and the panel still wants to list what is installed - with
# nothing marked active.
BASE = "base"


def cmd_instance(args):
    mods = {m.get("id", p.name): (p, m) for p, m in load_mods()}
    if args.mod == BASE:
        mod_path, manifest = None, {"name": "the base game"}
    elif args.mod in mods:
        mod_path, manifest = mods[args.mod]
    else:
        print("no mod '{}'. Known: {}".format(
            args.mod, ", ".join(sorted(mods) + [BASE])))
        return 1

    out = Path(args.out).resolve() if args.out else INSTANCE_ROOT / args.mod
    if out.exists():
        # Everything but the saves, which belong to whoever played them, and
        # the logs, which are how a problem gets diagnosed after the fact.
        for child in out.iterdir():
            if child.name in (SAVE_DIR, "logs"):
                continue
            if child.is_dir() and not child.is_symlink():
                shutil.rmtree(child)
            else:
                child.unlink()
    out.mkdir(parents=True, exist_ok=True)

    # Filters exist so a mod that does not work wholesale can be bisected:
    # apply only the databases, then only the XML, and so on, until the file
    # the 360 build cannot read is named.
    def wanted(rel):
        if args.include and not any(fnmatch.fnmatch(rel, g) for g in args.include):
            return False
        if args.exclude and any(fnmatch.fnmatch(rel, g) for g in args.exclude):
            return False
        return True

    overrides = {}
    skipped_by_filter = 0
    mod_data = (mod_path / "data") if mod_path else None
    if mod_data and mod_data.is_dir():
        for p, rel in rel_files(mod_data):
            key = "data/" + rel
            if wanted(key):
                overrides[key] = p
            else:
                skipped_by_filter += 1

    base = replaced = added = 0
    for p, rel in rel_files(GAME_ROOT):
        target = out / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        if rel in overrides:
            link_or_copy(overrides.pop(rel), target)
            replaced += 1
        else:
            link_or_copy(p, target)
            base += 1
    # Anything the mod brings that the base game does not have.
    for rel, p in overrides.items():
        target = out / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        link_or_copy(p, target)
        added += 1

    # A PS3 mod's own config files ask for "data/ps3/..." by name - the audio
    # and commentary system does, and failing those is what sent the game back
    # to the title screen after Start. Renaming the folder on import moves the
    # files but not the paths written inside them, so expose the tree under
    # both spellings. A junction costs nothing; a linked copy is the fallback
    # where junctions are not available.
    alias = out / "data" / "ps3"
    xenon = out / "data" / "xenon"
    aliased = None
    if xenon.is_dir() and not alias.exists():
        try:
            subprocess.run(["cmd", "/c", "mklink", "/J", str(alias), str(xenon)],
                           check=True, capture_output=True)
            aliased = "junction"
        except Exception:
            for p, rel in rel_files(xenon):
                t = alias / rel
                t.parent.mkdir(parents=True, exist_ok=True)
                link_or_copy(p, t)
            aliased = "linked copy"

    # The menu this port shows is not the one on the disc: the dead Xbox Live
    # icons are gone, the last two are text, and the JAMnet panel lists the
    # mods instead of apologising for the servers. It is applied here because
    # this is where the list of mods is known.
    front = []
    if not args.stock_menu:
        front = frontend.apply(out, [m for _p, m in load_mods()],
                               None if args.mod == BASE else args.mod)

    # Point this root at its own saves. A root built on its own keeps them
    # beside default.xex; a staged mod is handed a path back up to the one
    # saves folder the game shares. Either way it is written relative to the
    # root, so the folder can be moved and its saves travel with it.
    rel_saves = getattr(args, "saves", None) or SAVE_DIR
    (out / rel_saves).resolve().mkdir(parents=True, exist_ok=True)
    (out / SAVE_MARKER).write_text(rel_saves + "\n", encoding="utf-8")

    print("instance for '{}' ({})".format(manifest.get("name", args.mod), args.mod))
    print("  {} from the base game, {} replaced by the mod, {} added".format(
        base, replaced, added))
    if skipped_by_filter:
        print("  {} mod file(s) held back by the filter".format(skipped_by_filter))
    if aliased:
        print("  data/ps3 -> data/xenon ({})".format(aliased))
    for line in front:
        print("  {}".format(line))
    print("  {}".format(out))
    print("\nRun it with:")
    print('  .\\run.ps1 -GameRoot "{}"'.format(out))
    return 0


MOD_LIST = "mods.list"


def write_mod_list(roots, active, out):
    """Tell a game root about every other one, so the game can switch.

    Holding LT and pressing Y on the main menu opens the chooser - see
    src/mod_picker.cpp - and this file is the only thing it reads. Tab
    separated, because a mod's name has spaces in it and its paths might too.

    Six fields: the id, the game root, the name, and then the three things the
    chooser says about whichever one the cursor is on - version, author, and
    how many files it replaces. A field can be empty and is then just not
    shown; an older file with only three of them still reads.

    Saves are not in here - they follow from the note each root carries, so
    that they are right however the game was started and not only after a
    swap.
    """
    lines = ["# Written by tools/dlc.py stage. LT+Y on the main menu opens "
             "the chooser.",
             "# mod <id> <TAB> <root> <TAB> <name> <TAB> <version> <TAB> "
             "<author> <TAB> <files>",
             "active {}".format(active)]
    for mod_id, path, name, about in roots:
        lines.append("mod {}\t{}\t{}\t{}\t{}\t{}".format(
            mod_id, path, name, about.get("version") or "",
            about.get("author") or "",
            "" if about.get("files") in (None, "") else about["files"]))
    (Path(out) / MOD_LIST).write_text("\n".join(lines) + "\n", encoding="utf-8")


# Where a staged game goes, and what it looks like when it is there:
#
#     NBA JAM On Fire Edition PC/
#         nbajam_ofe.exe          the game
#         default.xex, data/      the base game, as the disc has it
#         saves/                  the base game's saves
#         saves/<name>/           one mod's saves
#         mods/<name>/            one mod's data
#         ui/                     the game's typefaces, unpacked
#
# One executable. It plays whatever sits beside it, so a mod is reached by
# pointing the same executable at its folder - which is all a swap does.
STAGE_ROOT = PARENT / "NBA JAM On Fire Edition PC"
BUILD_DIR = PORT / "out" / "build" / "local-relwithdebinfo"
MODS_DIR = "mods"


# The four TrueType faces the front end is drawn in live in one archive, in
# this order and with no names on them. The two the port draws with are the
# display face the menu items use and the text face under them; the other two
# are an italic and a wider Eurostile cut that nothing here needs.
#
# They are unpacked beside the executable rather than read from the archive at
# runtime, because the runtime wants a file it can hand to the font library,
# and because this way a mod folder does not need its own copy: every root
# shares the one executable, and so the one ui folder next to it.
UI_DIR = "ui"
FONT_AST = "data/xenon/fe/fonts/fonts.ast"
FONTS = {1: "display.ttf", 2: "text.ttf"}


def stage_fonts(out):
    """Unpack the game's own typefaces into <game>/ui."""
    src = out / FONT_AST
    if not src.is_file():
        return 0
    archive = ast_repack.Archive(src)
    (out / UI_DIR).mkdir(exist_ok=True)
    written = 0
    for index, name in FONTS.items():
        if index >= len(archive.entries):
            continue
        data = archive.read(archive.entries[index])
        if data[:4] not in (b"\x00\x01\x00\x00", b"true", b"OTTO"):
            continue        # not a TrueType file: leave it alone
        (out / UI_DIR / name).write_bytes(data)
        written += 1
    return written


# The help bar along the bottom of every screen draws the buttons it is
# talking about, and those pictures are textures in one archive, in this
# order. They are the game's own: the chooser the port adds uses the same
# ones, so its prompts are the prompts the rest of the game gives.
#
# Only the base game's copy is ever read. A mod's copy came from the PS3 and
# has PlayStation shapes in it, which would be wrong on a build that says A
# and B everywhere else.
GLYPH_AST = "data/xenon/fe/bounce/parts/helpbarcallout.ast"
GLYPHS = {8: "y", 10: "a", 11: "b", 14: "lt", 18: "dpad"}

# Pictures, in the plainest form the runtime can read without a decoder in
# it: a tag, the size, and the pixels.
GLYPH_MAGIC = b"NBTX"


def stage_glyphs(out):
    """Unpack the button pictures the help bar uses into <game>/ui."""
    src = out / GLYPH_AST
    if not src.is_file():
        return 0
    try:
        from PIL import Image
    except ImportError:
        print("  (no Pillow: the chooser will draw its own buttons)")
        return 0
    import io as _io

    archive = ast_textures.Ast(str(src))
    (out / UI_DIR).mkdir(exist_ok=True)
    written = 0
    index = 0
    for entry in archive.entries:
        blob = archive.read(entry)
        if ast_textures.kind(blob) != "xpr2":
            continue
        name = GLYPHS.get(index)
        index += 1
        if not name:
            continue
        try:
            dds, w, h = ast_textures.xpr2_to_dds(blob)
        except ValueError:
            continue
        img = Image.open(_io.BytesIO(dds)).convert("RGBA")
        body = GLYPH_MAGIC + struct.pack("<II", img.width, img.height)
        (out / UI_DIR / ("glyph_" + name + ".tex")).write_bytes(
            body + img.tobytes())
        written += 1
    return written


def stage_runtime(out, build):
    """Put the executable and what it needs beside the game."""
    if not (build / "nbajam_ofe.exe").is_file():
        raise SystemExit("no build in {} - build it first".format(build))
    copied = 0
    for f in sorted(build.iterdir()):
        if f.suffix.lower() in (".exe", ".dll") and f.is_file():
            shutil.copy2(f, out / f.name)
            copied += 1
    # The mod manager, if it has been built. It is what a player uses to put
    # a mod in; see tools/build_manager.py.
    manager = PORT / "out" / "manager" / "dist" / "NBA JAM Mod Manager.exe"
    if manager.is_file():
        shutil.copy2(manager, out / manager.name)
        copied += 1
    meta = PORT / "metadata"
    if meta.is_dir():
        if (out / "metadata").exists():
            shutil.rmtree(out / "metadata")
        shutil.copytree(meta, out / "metadata")
    return copied


def running_game():
    """Whether a staged game is open. Rebuilding one under itself leaves the
    folder half linked, and the game it breaks is the next one started, not
    the one running - which is a confusing hour to spend."""
    try:
        out = subprocess.run(["tasklist", "/fi", "imagename eq nbajam_ofe.exe"],
                             capture_output=True, text=True, timeout=20)
    except Exception:
        return False
    return "nbajam_ofe.exe" in out.stdout


def cmd_stage(args):
    """Lay the whole thing out the way someone would actually have it."""
    if running_game():
        print("the game is open - close it first, or the staged folder ends "
              "up half rebuilt")
        return 1
    mods = [(m.get("id", p.name), m) for p, m in load_mods()]
    out = Path(args.out).resolve() if args.out else STAGE_ROOT
    build = Path(args.build).resolve() if args.build else BUILD_DIR

    built = []
    for mod_id, manifest in [(BASE, {"name": "Base game"})] + mods:
        target = out if mod_id == BASE else out / MODS_DIR / mod_id
        print("\nbuilding {}".format(target))
        # Saves for a mod live with the game rather than with the mod, two
        # folders up from where the mod's own root is.
        saves = SAVE_DIR if mod_id == BASE else "../../{}/{}".format(
            SAVE_DIR, mod_id)
        sub = argparse.Namespace(mod=mod_id, out=str(target), include=None,
                                 exclude=None, stock_menu=args.stock_menu,
                                 saves=saves)
        if cmd_instance(sub):
            return 1
        built.append((mod_id, str(target), manifest.get("name", mod_id),
                      manifest))

    # Each one lists all of them and names itself as the one running, which is
    # how a swap knows where it is in the list.
    for mod_id, path, _name, _about in built:
        write_mod_list(built, mod_id, path)

    copied = stage_runtime(out, build)
    fonts = stage_fonts(out)
    buttons = stage_glyphs(out)
    print("\n{} game(s) staged, {} runtime file(s) copied, {} font(s), "
          "{} button picture(s)".format(len(built), copied, fonts, buttons))
    print("Play it by running:")
    print("  {}".format(out / "nbajam_ofe.exe"))
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("import", help="turn an extracted PS3 mod into a DLC folder")
    p.add_argument("source", help="a PS3 .pkg, or an extracted USRDIR "
                                  "(or a folder containing data/)")
    p.add_argument("--id")
    p.add_argument("--name")
    p.add_argument("--version")
    p.add_argument("--author")
    p.add_argument("--notes")
    p.add_argument("--force", action="store_true")
    p.set_defaults(func=cmd_import)

    p = sub.add_parser("list", help="list installed mods")
    p.set_defaults(func=cmd_list)

    p = sub.add_parser("instance", help="build a game root with a mod applied")
    p.add_argument("mod", help="a mod id, or '%s' for the game on its own" % BASE)
    p.add_argument("--out")
    p.add_argument("--include", action="append", metavar="GLOB",
                   help="only apply mod files matching this (repeatable)")
    p.add_argument("--stock-menu", action="store_true",
                   help="leave the front end exactly as the disc has it")
    p.add_argument("--exclude", action="append", metavar="GLOB",
                   help="do not apply mod files matching this (repeatable)")
    p.set_defaults(func=cmd_instance)

    p = sub.add_parser("stage", help="lay out a playable game folder with every mod")
    p.add_argument("--out", help="where it goes (default: a folder beside DLC/)")
    p.add_argument("--build", help="where the built executable is")
    p.add_argument("--stock-menu", action="store_true",
                   help="leave the front end exactly as the disc has it")
    p.set_defaults(func=cmd_stage)

    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
