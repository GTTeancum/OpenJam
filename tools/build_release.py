#!/usr/bin/env python3
"""Package a release: the port, the manager, and instructions. No game data.

    python tools/build_release.py --version 1.1                    (Windows)
    python3 tools/build_release.py --version 1.1 --platform linux  (in WSL)

Leaves `out/release/OpenJam-v<version>-win64.zip` (or
`OpenJam-v<version>-linux-x86_64.tar.gz`) and the folder it was made from, so
the folder can be looked at before the archive goes anywhere.

What goes in is listed in RELEASE.md and enforced here: the game, the three
libraries it links, the manager (Windows) or a launcher (Linux), the
achievement data, and a readme written for somebody who has never seen any of
this. Everything else in a staged game folder - the game's own files, mods,
saves - belongs to the player and is never in the download.

The Linux package is made under Linux (WSL will do) because its programs are
stripped of their debugging information there, which takes them from about
200 MB to a fraction of that, and because a tar made there keeps them
executable.
"""

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tarfile
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
PORT = HERE.parent
PARENT = PORT.parent
BUILD = PORT / "out" / "build" / "local-relwithdebinfo"
MANAGER = PORT / "out" / "manager" / "dist" / "NBA JAM Mod Manager.exe"
LINUX_BUILD = PORT / "out" / "build" / "local-linux-relwithdebinfo"
# The runtime and profiler client come from the SDK. The graphics plugin is
# taken from beside the game, where the build copies it: it has to be the one
# rebuilt with tools/sdk-patches applied (see BUILDING.md), or matches draw
# unlit with no scoreboard.
LINUX_SDK = PORT / "_deps" / "rexglue-dist" / "linux-amd64" / "lib"
OUT = PORT / "out" / "release"

# Everything the download holds, and nothing else.
SHIPPED = [
    ("nbajam_ofe.exe", BUILD / "nbajam_ofe.exe", "the game"),
    ("rexruntimerd.dll", BUILD / "rexruntimerd.dll", "runtime"),
    ("rexgpu-xenosrd.dll", BUILD / "rexgpu-xenosrd.dll", "graphics"),
    ("TracyClientrd.dll", BUILD / "TracyClientrd.dll", "profiler client"),
    ("NBA JAM Mod Manager.exe", MANAGER, "setup and mods"),
    ("gamecontrollerdb.txt", PORT / "res" / "gamecontrollerdb.txt",
     "controller mappings"),
    ("gamecontrollerdb.LICENSE.txt",
     PORT / "res" / "gamecontrollerdb.LICENSE.txt", "its licence"),
]

LINUX_SHIPPED = [
    ("nbajam_ofe", LINUX_BUILD / "nbajam_ofe", "the game"),
    ("librexruntimerd.so", LINUX_SDK / "librexruntimerd.so", "runtime"),
    ("librexgpu-xenosrd.so", LINUX_BUILD / "librexgpu-xenosrd.so", "graphics"),
    ("libTracyClientrd.so", LINUX_SDK / "libTracyClientrd.so",
     "profiler client"),
    ("nbajam.sh", PORT / "res" / "linux" / "nbajam.sh", "start here"),
    ("mods.sh", PORT / "res" / "linux" / "mods.sh", "install mods"),
    ("gamecontrollerdb.txt", PORT / "res" / "gamecontrollerdb.txt",
     "controller mappings"),
    ("gamecontrollerdb.LICENSE.txt",
     PORT / "res" / "gamecontrollerdb.LICENSE.txt", "its licence"),
]
LINUX_EXECUTABLE = {"nbajam_ofe", "nbajam.sh", "mods.sh"}
LINUX_STRIPPED = {"nbajam_ofe", "librexruntimerd.so", "librexgpu-xenosrd.so",
                  "libTracyClientrd.so"}
# The Mod Manager's own code, for mods.sh (installing mods from a terminal,
# since the manager's window is Windows-only so far) and for the launcher's
# first run (the game's typefaces and button art, if Pillow is there). Only
# the standard library is needed beyond these.
LINUX_SETUP = ["mods_cli.py", "modkit.py", "ps3_pkg_extract.py", "dlc.py",
               "ast_repack.py", "ast_textures.py", "commentary.py",
               "frontend.py", "apt.py", "apt_movie.py", "locdb.py",
               "roster_update.py", "verify_assets.py"]

README = """NBA JAM: On Fire Edition - PC port (OpenJam) v{version}
=========================================================

This is the game recompiled to run natively on Windows. It is not an
emulator, and it does not include the game. This is a complete download: if
you have an older version, set this one up in a new folder (see UPGRADING).

WHAT YOU NEED
-------------

Your own copy of NBA JAM: On Fire Edition, bought on Xbox Live Arcade. On the
console's drive it is a single file with a long name made of numbers and
letters, and no file extension. It is about 850 MB.

You also need Windows 10 or 11, 64-bit, and a graphics card that supports
Direct3D 12. Anything from the last ten years will do. Nothing else has to be
installed - the libraries the game needs are in this folder already.

INSTALLING
----------

1. Unzip this download into a folder of its own, for example
   C:\\Games\\NBA JAM. Avoid Program Files, which Windows will not let the
   game write into.
2. Copy your game file (the 850 MB one with no extension) into that folder,
   next to "NBA JAM Mod Manager.exe".
3. Run "NBA JAM Mod Manager.exe". It finds your copy of the game and offers
   to unpack it. Press "Unpack it". That takes under a minute and about
   850 MB beside it; your original file is left alone.
4. Press Play.

If Windows says it protected your PC, press "More info" and then "Run
anyway". The programs are not signed, which is all that warning means.

The game opens in a window. A controller is recommended; the menus also work
with the keyboard. Options > Settings > Video switches to fullscreen and sets
the resolution.

UPGRADING
---------

Set the new version up in a new folder as above, then install your mods
again from their .pkg files. Your progress in the base game carries over by
itself (see WHERE THINGS GO). To keep your progress in mods, copy the saves
folder from the old folder into the new one before installing them.

MODS
----

The PlayStation 3 modding scene for this game is very much alive, and those
mods run here. They are published as .pkg files, usually in two parts ("Part_1"
and "Part_2") - keep both in the same folder and pick either one. Press
"Install a mod" and pick it. Each mod takes two or three minutes and about
850 MB. All three of the published mods, one after another, take around ten
minutes on a fast machine; allow fifteen on an ordinary one.

Installed mods appear on the game's own main menu, in the panel on the right.
Hold the left trigger and press Y there to open the list, move with the d-pad,
and press A to load one. The game restarts into it, which takes about fifteen
seconds. Every mod keeps its own saves, records and unlocked players, so
switching between them never mixes anything up.

STEAM DECK AND LINUX
--------------------

This Windows download runs through Proton, mods and all. In desktop mode, add
"NBA JAM Mod Manager.exe" to Steam as a non-Steam game and set it to run with
Proton under Properties > Compatibility; add nbajam_ofe.exe the same way to
go straight into the game. On a Deck it starts fullscreen at the Deck's own
resolution. There is also a native Linux download, which installs mods from
a terminal instead of a window.

WHERE THINGS GO
---------------

Almost everything stays in this folder. Nothing is written to your registry,
and nothing here talks to the internet.

    data/, default.xex      the game, unpacked from your copy
    mods/                   one folder per mod
    saves/                  your mods' saves, one folder per mod
    logs/                   what the game wrote down, if something goes wrong

The one exception is the base game's saves, which go in your Documents
folder under nbajam_ofe, with a cache that makes later launches quicker.

IF SOMETHING GOES WRONG
-----------------------

The newest file in logs/ says what happened, and crash_stack.txt appears
beside the game if it falls over. Both are worth attaching to a bug report.

If a mod is ever short of something the game needs, the game says so on screen
and offers to start again rather than closing or sitting there.

---

Not affiliated with or endorsed by EA, the NBA, or Microsoft. For
preservation and educational purposes. The game data is yours and never
leaves your machine.

{repo}
"""

LINUX_README = """NBA JAM: On Fire Edition - PC port (OpenJam) v{version} for Linux
=====================================================================

This is the game recompiled to run natively on Linux. It is not an emulator,
and it does not include the game. This is a complete download.

WHAT YOU NEED
-------------

Your own copy of NBA JAM: On Fire Edition, bought on Xbox Live Arcade. On the
console's drive it is a single file with a long name made of numbers and
letters, and no file extension. It is about 850 MB.

You also need 64-bit Linux on an x86 PC and a graphics driver with Vulkan
(the standard Mesa drivers for AMD and Intel, or NVIDIA's own). Nothing else
has to be installed - the libraries the game needs are in this folder.

INSTALLING
----------

1. Unpack this download where you want the game to live:

       tar -xzf OpenJam-v{version}-linux-x86_64.tar.gz

2. Copy your game file (the 850 MB one with no extension) into the new
   OpenJam-v{version}-linux-x86_64 folder, next to nbajam.sh.
3. Run nbajam.sh, from a terminal in that folder:

       ./nbajam.sh

   or by double-clicking it and choosing Run. The first time, it unpacks the
   game - under a minute and about 850 MB beside it; your original file is
   left alone - and then starts it. After that it goes straight in.

The game opens in a window. A controller is recommended; the menus also work
with the keyboard. Options > Settings > Video switches to fullscreen and sets
the resolution.

MODS
----

The PlayStation 3 modding scene for this game is very much alive, and those
mods run here. They are published as .pkg files, usually in two parts ("Part_1"
and "Part_2") - keep both in the same folder and give either one. Run the
game once first so it is unpacked, then, from a terminal in this folder:

    ./mods.sh install ~/Downloads/NBA_JAM_2020s_On_Fire_Edition_v6.0_by_johnz1-Part_1.pkg

Each mod takes two or three minutes and about 850 MB. Then:

    ./mods.sh list              what is installed
    ./mods.sh use <id>          which one the game starts in
    ./mods.sh remove <id>       take one out again (its saves are kept)

Installed mods also appear on the game's own main menu, in the panel on the
right. Hold the left trigger and press Y there to open the list, move with
the d-pad, and press A to load one. The game restarts into it, which takes
about fifteen seconds. Every mod keeps its own saves, records and unlocked
players.

mods.sh needs Python 3, which almost every Linux system and the Steam Deck
already have. Nothing else has to be installed.

STEAM DECK
----------

Either download works on a Deck. This native one: add nbajam.sh to Steam as
a non-Steam game, and install mods from desktop mode with mods.sh. The
Windows download through Proton has the Mod Manager's window instead; its
readme says how to set it up.

WHERE THINGS GO
---------------

Almost everything stays in this folder, and nothing here talks to the
internet.

    data/, default.xex      the game, unpacked from your copy
    mods/                   one folder per mod
    saves/                  your mods' saves, one folder per mod
    ui/                     the game's typefaces, for the port's own screens
    logs/                   what the game wrote down, if something goes wrong

The one exception is the base game's saves, which go in
~/.local/share/nbajam_ofe, with a cache that makes later launches quicker.

The typefaces are unpacked only if the machine has Python 3 with Pillow;
without them the port's own screens use a plain face and nothing else
changes.

IF SOMETHING GOES WRONG
-----------------------

The newest file in logs/ says what happened, and crash_stack.txt appears
beside the game if it falls over. Both are worth attaching to a bug report.

This is the first native Linux release. It has been played through whole
matches on a software graphics driver but not yet on a range of real
graphics cards, so reports from real hardware are very welcome.

---

Not affiliated with or endorsed by EA, the NBA, or Microsoft. For
preservation and educational purposes. The game data is yours and never
leaves your machine.

{repo}
"""

REPO = "https://github.com/GTTeancum/OpenJam"


def show(folder, target, what):
    path = folder / target
    size = (sum(f.stat().st_size for f in path.rglob("*") if f.is_file())
            if path.is_dir() else path.stat().st_size)
    print("  %-30s %6.1f MB  %s" % (target, size / 1048576, what))


def fresh(name):
    folder = OUT / name
    if folder.exists():
        shutil.rmtree(folder)
    folder.mkdir(parents=True)
    return folder


def copy_metadata(folder):
    meta = PORT / "metadata"
    if meta.is_dir():
        shutil.copytree(meta, folder / "metadata")
        show(folder, "metadata", "achievements")


def write_checksum(archive, name):
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    print("\n%s" % archive)
    print("  %.0f MB" % (archive.stat().st_size / 1048576))
    print("  sha256 %s" % digest)
    (OUT / (name + ".sha256")).write_text(
        "%s  %s\n" % (digest, archive.name), encoding="utf-8")


def missing_from(shipped, hint):
    missing = [str(src) for _name, src, _what in shipped if not src.is_file()]
    if missing:
        print("not built yet:")
        for m in missing:
            print("   " + m)
        print("\n" + hint)
    return bool(missing)


def windows(version):
    if missing_from(SHIPPED, "run the build and tools/build_manager.py first"):
        return 1
    name = "OpenJam-v%s-win64" % version
    folder = fresh(name)
    for target, src, what in SHIPPED:
        shutil.copy2(src, folder / target)
        show(folder, target, what)
    copy_metadata(folder)
    (folder / "README.txt").write_text(
        README.format(version=version, repo=REPO).replace("\n", "\r\n"),
        encoding="utf-8")
    show(folder, "README.txt", "instructions")

    archive = OUT / (name + ".zip")
    if archive.exists():
        archive.unlink()
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as zf:
        for path in sorted(folder.rglob("*")):
            if path.is_file():
                zf.write(path, str(Path(name) / path.relative_to(folder)))
    write_checksum(archive, name)
    return 0


def linux(version):
    if os.name == "nt" or not shutil.which("strip"):
        print("run this under Linux or WSL: it needs strip, and a tar made")
        print("there keeps the programs executable")
        return 1
    if missing_from(LINUX_SHIPPED,
                    "build with the local-linux-relwithdebinfo preset first"):
        return 1
    name = "OpenJam-v%s-linux-x86_64" % version
    folder = fresh(name)
    for target, src, what in LINUX_SHIPPED:
        dst = folder / target
        shutil.copyfile(src, dst)
        if target in LINUX_STRIPPED:
            subprocess.run(["strip", "--strip-debug", str(dst)], check=True)
        os.chmod(dst, 0o755 if target in LINUX_EXECUTABLE else 0o644)
        show(folder, target, what)
    setup = folder / "setup"
    setup.mkdir()
    for f in LINUX_SETUP:
        shutil.copyfile(HERE / f, setup / f)
    show(folder, "setup", "mod installer and first-run helpers")
    copy_metadata(folder)
    (folder / "README.txt").write_text(
        LINUX_README.format(version=version, repo=REPO), encoding="utf-8")
    show(folder, "README.txt", "instructions")

    archive = OUT / (name + ".tar.gz")
    if archive.exists():
        archive.unlink()

    def tidy(info):
        # Nobody's user or group, and the modes set above rather than
        # whatever the filesystem it was assembled on reports.
        info.uid = info.gid = 0
        info.uname = info.gname = ""
        base = info.name.rsplit("/", 1)[-1]
        if info.isdir():
            info.mode = 0o755
        else:
            info.mode = 0o755 if base in LINUX_EXECUTABLE else 0o644
        return info

    with tarfile.open(archive, "w:gz") as tf:
        tf.add(folder, arcname=name, filter=tidy)
    write_checksum(archive, name)
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--version", default="1.0")
    ap.add_argument("--platform", choices=("windows", "linux"),
                    default="windows" if os.name == "nt" else "linux")
    args = ap.parse_args()
    if args.platform == "linux":
        return linux(args.version)
    return windows(args.version)


if __name__ == "__main__":
    sys.exit(main())
