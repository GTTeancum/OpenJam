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
- Textures do **not**. The PS3 build stores them as DDS inside the `.ast`
  archives and this build expects XPR. Applying the mod's front-end archives
  leaves the menus black: the game fails the load quietly rather than drawing
  garbage. Converting them is the open piece of work. The PS3 and
Xbox 360 releases of NBA JAM: On Fire Edition are the same game on the same
big-endian PowerPC architecture and their data trees line up almost exactly:
`data/ps3/...` against `data/xenon/...`, with 170 files byte-identical between
a modded PS3 build and the stock 360 build. The differences that do exist are
the mod's own content plus a handful of platform files (PS3 `.gtf` boot
textures against 360 `.xpr`, PS3 save icons), which are left behind.

`instance` builds a complete game root for a chosen mod: every base game file,
with the mod's files in place of the ones it replaces. It uses hard links, so
an 850 MB mod instance costs kilobytes and takes seconds rather than copying.
Point `--game_data_root` at the result.

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
import sys
from pathlib import Path

# The port lives in <parent>/port; the game data and DLC live beside it.
PORT = Path(__file__).resolve().parent.parent
PARENT = PORT.parent
GAME_ROOT = PARENT / "Root"
DLC_ROOT = PARENT / "DLC"
INSTANCE_ROOT = PARENT / "instances"

SCHEMA = 1

# PS3-only files with no meaning on this build. `.gtf` is the PS3 texture
# container, where the 360 uses `.xpr`; saveload icons are PS3 save metadata.
SKIP_SUFFIXES = (".gtf",)
SKIP_PATTERNS = (re.compile(r"^xenon/saveload/"),)


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

    copied = skipped = 0
    for src_path, rel in rel_files(data):
        mapped = map_path(rel)
        if mapped.endswith(SKIP_SUFFIXES) or any(p.match(mapped) for p in SKIP_PATTERNS):
            skipped += 1
            continue
        target = dest / "data" / mapped
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src_path, target)
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


def cmd_instance(args):
    mods = {m.get("id", p.name): (p, m) for p, m in load_mods()}
    if args.mod not in mods:
        print("no mod '{}'. Known: {}".format(args.mod, ", ".join(sorted(mods)) or "none"))
        return 1
    mod_path, manifest = mods[args.mod]

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
    mod_data = mod_path / "data"
    if mod_data.is_dir():
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

    print("instance for '{}' ({})".format(manifest.get("name", args.mod), args.mod))
    print("  {} from the base game, {} replaced by the mod, {} added".format(
        base, replaced, added))
    if skipped_by_filter:
        print("  {} mod file(s) held back by the filter".format(skipped_by_filter))
    print("  {}".format(out))
    print("\nRun it with:")
    print('  .\\run.ps1 -GameRoot "{}"'.format(out))
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
    p.add_argument("mod")
    p.add_argument("--out")
    p.add_argument("--include", action="append", metavar="GLOB",
                   help="only apply mod files matching this (repeatable)")
    p.add_argument("--exclude", action="append", metavar="GLOB",
                   help="do not apply mod files matching this (repeatable)")
    p.set_defaults(func=cmd_instance)

    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
