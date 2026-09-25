#!/usr/bin/env python3
"""Package a release: the port, the manager, and instructions. No game data.

    python tools/build_release.py --version 1.0

Leaves `out/release/OpenJam-v<version>-win64.zip` and the folder it was made
from, so the folder can be looked at before the zip goes anywhere.

What goes in is listed in RELEASE.md and enforced here: the game, the three
libraries it links, the manager, the achievement data, and a readme written
for somebody who has never seen any of this. Everything else in a staged game
folder - the game's own files, mods, saves - belongs to the player and is
never in the download.
"""

import argparse
import hashlib
import os
import shutil
import sys
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
PORT = HERE.parent
PARENT = PORT.parent
BUILD = PORT / "out" / "build" / "local-relwithdebinfo"
MANAGER = PORT / "out" / "manager" / "dist" / "NBA JAM Mod Manager.exe"
OUT = PORT / "out" / "release"

# Everything the download holds, and nothing else.
SHIPPED = [
    ("nbajam_ofe.exe", BUILD / "nbajam_ofe.exe", "the game"),
    ("rexruntimerd.dll", BUILD / "rexruntimerd.dll", "runtime"),
    ("rexgpu-xenosrd.dll", BUILD / "rexgpu-xenosrd.dll", "graphics"),
    ("TracyClientrd.dll", BUILD / "TracyClientrd.dll", "profiler client"),
    ("NBA JAM Mod Manager.exe", MANAGER, "setup and mods"),
]

README = """NBA JAM: On Fire Edition - PC port (OpenJam) v{version}
=========================================================

This is the game recompiled to run natively on Windows. It is not an
emulator, and it does not include the game.

WHAT YOU NEED
-------------

Your own copy of NBA JAM: On Fire Edition, bought on Xbox Live Arcade. On the
console's drive it is a single file with a long name made of numbers and
letters, and no file extension. It is about 850 MB. Copy that one file into
this folder, next to "NBA JAM Mod Manager.exe".

You also need Windows 10 or 11, 64-bit, and a graphics card that supports
Direct3D 12. Anything from the last ten years will do. Nothing else has to be
installed - the libraries the game needs are in this folder already.

SETTING IT UP
-------------

Run "NBA JAM Mod Manager.exe". It will find your copy of the game, ask once,
and unpack it. That takes a couple of minutes and about 850 MB. When it is
done, press Play.

The game opens in a window. A controller is recommended; the menus also work
with the keyboard.

MODS
----

The PlayStation 3 modding scene for this game is very much alive, and those
mods run here. They are published as .pkg files. Download one, press "Install
a mod" and pick it. It takes about two minutes and 700 MB.

Installed mods appear on the game's own main menu, in the panel on the right.
Hold the left trigger and press Y there to open the list, move with the d-pad,
and press A to load one. The game restarts into it, which takes about fifteen
seconds. Every mod keeps its own saves, records and unlocked players, so
switching between them never mixes anything up.

WHERE THINGS GO
---------------

Everything stays in this folder. Nothing is written to your documents, your
registry or anywhere else, and nothing here talks to the internet.

    data/, default.xex      the game, unpacked from your copy
    mods/                   one folder per mod
    saves/                  your saves, one folder per mod
    logs/                   what the game wrote down, if something goes wrong

IF SOMETHING GOES WRONG
-----------------------

The newest file in logs/ says what happened, and crash_stack.txt appears
beside the game if it falls over. Both are worth attaching to a bug report.

Known: one of the three published mods, the Legends edition, crashes on the
way into a match. The other two play.

---

Not affiliated with or endorsed by EA, the NBA, or Microsoft. For
preservation and educational purposes. The game data is yours and never
leaves your machine.

{repo}
"""

REPO = "https://github.com/GTTeancum/OpenJam"


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--version", default="1.0")
    args = ap.parse_args()

    missing = [str(src) for _name, src, _what in SHIPPED if not src.is_file()]
    if missing:
        print("not built yet:")
        for m in missing:
            print("   " + m)
        print("\nrun the build and tools/build_manager.py first")
        return 1

    name = "OpenJam-v%s-win64" % args.version
    folder = OUT / name
    if folder.exists():
        shutil.rmtree(folder)
    folder.mkdir(parents=True)

    for target, src, what in SHIPPED:
        shutil.copy2(src, folder / target)
        print("  %-26s %6.1f MB  %s"
              % (target, (folder / target).stat().st_size / 1048576, what))

    meta = PORT / "metadata"
    if meta.is_dir():
        shutil.copytree(meta, folder / "metadata")
        print("  %-26s %6.1f MB  %s" % ("metadata/", sum(
            f.stat().st_size for f in (folder / "metadata").rglob("*")
            if f.is_file()) / 1048576, "achievements"))

    (folder / "README.txt").write_text(
        README.format(version=args.version, repo=REPO).replace("\n", "\r\n"),
        encoding="utf-8")
    print("  %-26s %6.1f KB  %s"
          % ("README.txt", (folder / "README.txt").stat().st_size / 1024,
             "instructions"))

    archive = OUT / (name + ".zip")
    if archive.exists():
        archive.unlink()
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as zf:
        for path in sorted(folder.rglob("*")):
            if path.is_file():
                zf.write(path, str(Path(name) / path.relative_to(folder)))

    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    print("\n%s" % archive)
    print("  %.0f MB" % (archive.stat().st_size / 1048576))
    print("  sha256 %s" % digest)
    (OUT / (name + ".sha256")).write_text(
        "%s  %s\n" % (digest, archive.name), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
