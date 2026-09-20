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
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ast_repack
import frontend

# The port lives in <parent>/port; the game data and DLC live beside it.
PORT = Path(__file__).resolve().parent.parent
PARENT = PORT.parent
GAME_ROOT = PARENT / "Root"
DLC_ROOT = PARENT / "DLC"
INSTANCE_ROOT = PARENT / "instances"

# Saves live in the game folder, one directory per mod, and each game root
# carries a note saying which is its own. src/mod_saves.cpp reads it, so a mod
# added later needs no entry anywhere: its root is built with the note in it
# and its saves follow. They are kept out of the roots themselves because
# `roots` deletes and rebuilds those, and a save is the one thing here that
# cannot be rebuilt.
SAVE_ROOT = PARENT / "saves"
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


def repack_archive(src, dst):
    """True when src was a PS3 archive and dst now holds the converted form."""
    try:
        return ast_repack.repack(str(src), str(dst), quiet=True)
    except Exception:
        if Path(dst).exists():
            Path(dst).unlink()
        return False


def cmd_import(args):
    src = Path(args.source).resolve()
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
                repack_archive(src_path, target)):
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
        shutil.rmtree(out)
    out.mkdir(parents=True)

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

    # Point this root at its own saves, in the game folder rather than in the
    # root - `roots` deletes and rebuilds roots, and a save is the one thing
    # here that cannot be rebuilt. Written relative where it can be, so moving
    # the game folder keeps the saves attached to it.
    saves = SAVE_ROOT / args.mod
    saves.mkdir(parents=True, exist_ok=True)
    try:
        note = os.path.relpath(saves, out)
    except ValueError:
        note = str(saves)
    (out / SAVE_MARKER).write_text(note + "\n", encoding="utf-8")

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

    Pressing Y on the main menu moves to the next root in this file - see
    src/mod_swap.cpp, which is the only thing that reads it. Tab separated,
    because a mod's name has spaces in it and its paths might too.

    Three fields: the id, the game root and the name. Saves are not in here -
    they follow from the `mod.id` each root carries, so that they are right
    however the game was started and not only after a swap.
    """
    lines = ["# Written by tools/dlc.py roots. Y on the main menu moves down "
             "this list.", "active {}".format(active)]
    for mod_id, path, name in roots:
        lines.append("mod {}\t{}\t{}".format(mod_id, path, name))
    (Path(out) / MOD_LIST).write_text("\n".join(lines) + "\n", encoding="utf-8")


def cmd_roots(args):
    """Build a game root for the base game and for every mod, ready to switch."""
    mods = [(m.get("id", p.name), m) for p, m in load_mods()]
    out = Path(args.out).resolve() if args.out else INSTANCE_ROOT
    plan = [(BASE, {"name": "Base game"})] + mods

    built = []
    for mod_id, manifest in plan:
        target = out / mod_id
        print("\nbuilding {}".format(target))
        sub = argparse.Namespace(mod=mod_id, out=str(target), include=None,
                                 exclude=None, stock_menu=args.stock_menu)
        if cmd_instance(sub):
            return 1
        built.append((mod_id, str(target), manifest.get("name", mod_id)))

    for mod_id, path, _name in built:
        write_mod_list(built, mod_id, path)
    print("\n{} root(s), each listing the others in {}".format(len(built), MOD_LIST))
    print("Start with:")
    print('  .\\run.ps1 -GameRoot "{}"'.format(built[0][1]))
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("import", help="turn an extracted PS3 mod into a DLC folder")
    p.add_argument("source", help="the extracted USRDIR (or a folder containing data/)")
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

    p = sub.add_parser("roots", help="build every root and let the game switch")
    p.add_argument("--out", help="where the roots go (default: instances/)")
    p.add_argument("--stock-menu", action="store_true",
                   help="leave the front end exactly as the disc has it")
    p.set_defaults(func=cmd_roots)

    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
