# Release manifest

What a released build contains, what the person running it has to provide, and
what is still missing before it can go out.

The shape of it: **we ship code, they bring the game.** Nothing in the release
is EA's. The player points it at the Xbox Live Arcade copy they already own,
and it builds a playable game folder out of that. Mods are downloaded by the
player from wherever they are published and installed through the manager.

---

## 1. What ships

Two archives, one per system, each unpacking to a folder the player chooses.
Each is a complete download, not an update to an earlier one.

**Windows**, `OpenJam-v<version>-win64.zip`, 55 MB, about 105 MB unpacked:

| File | Size | What it is |
| --- | --- | --- |
| `nbajam_ofe.exe` | 57 MB | The game. The recompiled title plus this port's own code. |
| `rexruntimerd.dll` | 11 MB | ReXGlue runtime. |
| `rexgpu-xenosrd.dll` | 2.9 MB | The Xenos GPU translation layer. |
| `TracyClientrd.dll` | 0.2 MB | Profiler client the runtime links against. |
| `NBA JAM Mod Manager.exe` | 34 MB | Setup and mod installation. The only thing a player needs to open. |
| `gamecontrollerdb.txt` + licence | 0.6 MB | SDL's community controller mappings. |
| `metadata/` | 4 KB | Achievement definitions. |
| `README.txt` | — | Installing, mods, Steam Deck, where things go. |

**Linux**, `OpenJam-v<version>-linux-x86_64.tar.gz`, 24 MB, about 80 MB
unpacked. The programs are stripped of debugging information, and the
graphics plugin is the one rebuilt from the patched SDK (BUILDING.md):

| File | Size | What it is |
| --- | --- | --- |
| `nbajam_ofe` | 55 MB | The game. Finds its libraries beside it. |
| `librexruntimerd.so` | 15 MB | ReXGlue runtime. |
| `librexgpu-xenosrd.so` | 6 MB | The Xenos GPU translation layer, patched. |
| `libTracyClientrd.so` | 0.5 MB | Profiler client the runtime links against. |
| `nbajam.sh` | — | Start here: unpacks the game on the first run, then plays the chosen mod. |
| `mods.sh` | — | Installs, lists, removes and picks mods from a terminal. |
| `setup/` | 0.2 MB | The Mod Manager's own Python code, which both scripts use. Standard library only; PS3 packages are opened with the system's OpenSSL. |
| `gamecontrollerdb.txt` + licence, `metadata/`, `README.txt` | | As on Windows. |

Nothing else. No game data, no mods, no save files.

## 2. What the player provides

**Their own copy of the game.** NBA JAM: On Fire Edition was sold on Xbox Live
Arcade; a purchased copy sits on the console's drive as one signed container
file with a 42-character hexadecimal name and no extension. Ours reads:

```
magic         LIVE
content type  0x000D0000   (Xbox Live Arcade title)
title id      0x584111EC   (NBA JAM: On Fire Edition)
display name  NBA JAM: On Fire Edition
size          851 MB
```

They copy that one file off their console and hand it to the manager.

**Mods, if they want any.** These are published as PlayStation 3 packages -
`.pkg` files, typically one small part and one large one, the large one being
the mod. The player downloads them; the manager opens them, converts the
artwork to the format this build reads, and installs them. The three that
exist today are johnz1's 1990s, 2020s and Legends editions.

## 3. First run

The manager opens, finds no game beside it, finds the container instead, and
offers to unpack it. One button. Then, without further questions:

1. **Unpack the container.** The game does this itself - `nbajam_ofe.exe
   --unpack <container>` - using the container reader the runtime already
   carries; the manager runs it and draws the progress bar. Out comes
   `default.xex` and the `data/` tree, about 840 MB, in a few seconds.
2. **Unpack the two typefaces and the five button pictures** the port draws
   its own screens with, out of the game's own archives, into `ui/`.
3. **Rewrite the front end.** The dead Xbox Live entries come out of the main
   menu, two icons become text, and the JAMnet panel becomes the mod list.
   The untouched originals are kept in `ui.original/` so the list can be
   redrawn later when mods are added or removed.
4. **Write `mods.list` and `saves.path`,** and make `saves/`.

The result is a folder the game runs from with no arguments.

## 4. What ends up on the player's disk

| | Size |
| --- | --- |
| The release itself | 105 MB |
| The game, unpacked from their container | 845 MB |
| Each mod installed | about 700 MB |

A mod is built as a game folder of its own, sharing every file it does not
change with the base game through hard links. It costs 700 MB rather than 845
because the files it replaces are most of the art; a mod that only changed the
rosters would cost a few megabytes. Saves are never shared: each mod keeps its
own under `saves/<mod>`, because the same save would unlock players in one mod
that were never played in another.

The finished layout:

```
NBA JAM On Fire Edition PC/
    nbajam_ofe.exe, *.dll          the game
    NBA JAM Mod Manager.exe        setup and mods
    default.xex, data/             from the player's container
    metadata/                      achievements
    ui/                            typefaces and button art
    ui.original/                   the front end as it arrived
    mods/<mod>/                    one folder per mod
    saves/                         the base game's saves
    saves/<mod>/                   one folder per mod
    mods.list, saves.path          what the game reads to find the rest
```

## 5. Building a release from this repository

Windows:

```
cmake --build --preset local-relwithdebinfo    # the game
python tools/build_manager.py                  # the manager, one file
python tools/build_release.py --version 1.1    # out/release/OpenJam-v1.1-win64.zip
```

Linux, in WSL, after rebuilding the SDK's graphics plugin as BUILDING.md says:

```
cmake --build --preset local-linux-relwithdebinfo
python3 tools/build_release.py --version 1.1 --platform linux
```

Each leaves the archive, the folder it was made from and a `.sha256` beside
it in `out/release/`. Before publishing, install each one the way a player
would: unpack it into an empty folder, add the container, set it up through
the manager or `nbajam.sh`, install the three published mods, and play a
match in the base game and in a mod.

## 6. Still to build

| | Why it matters | Size of the job |
| --- | --- | --- |
Nothing outstanding for a first release.

## 7. What a player needs

- Windows 10 or 11, 64-bit, with a GPU with Direct3D 12. Tested on an AMD
  Radeon 780M.
- Or 64-bit Linux with a Vulkan driver. Tested on Mesa's software driver in
  WSL; Python 3 for installing mods.
- About 1 GB free, plus 700 MB per mod.
- A controller. The game reads one; the menus also take the keyboard.

The game opens in a 1280x720 window. It is not fullscreen by default and does
not take the screen.
