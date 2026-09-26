# Release manifest

What a released build contains, what the person running it has to provide, and
what is still missing before it can go out.

The shape of it: **we ship code, they bring the game.** Nothing in the release
is EA's. The player points it at the Xbox Live Arcade copy they already own,
and it builds a playable game folder out of that. Mods are downloaded by the
player from wherever they are published and installed through the manager.

---

## 1. What ships

One archive, about 105 MB, that unpacks to a folder the player chooses.

| File | Size | What it is |
| --- | --- | --- |
| `nbajam_ofe.exe` | 57 MB | The game. The recompiled title plus this port's own code. |
| `rexruntimerd.dll` | 11 MB | ReXGlue runtime. |
| `rexgpu-xenosrd.dll` | 2.9 MB | The Xenos GPU translation layer. |
| `TracyClientrd.dll` | 0.2 MB | Profiler client the runtime links against. |
| `NBA JAM Mod Manager.exe` | 34 MB | Setup and mod installation. The only thing a player needs to open. |
| `metadata/` | 164 KB | Achievement definitions and icons, lifted from the executable. |
| `README.txt` | — | What to do on first run, in plain language. *(to write)* |

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

```
cmake --build --preset local-relwithdebinfo    # the game
python tools/build_manager.py                  # the manager, one file
python tools/dlc.py stage                      # a staged folder to test with
```

`build_manager.py` puts `NBA JAM Mod Manager.exe` in the staged folder as well,
so the staged folder is what a release looks like once the game data is in it.
The release archive is that folder with `data/`, `default.xex`, `mods/`,
`saves/`, `ui/`, `ui.original/`, `logs/` and `mods.list` removed.

## 6. Still to build

| | Why it matters | Size of the job |
| --- | --- | --- |
Nothing outstanding for a first release.

## 7. What a player needs

- Windows 10 or 11, 64-bit.
- A GPU with Direct3D 12. Tested on an AMD Radeon 780M.
- About 1 GB free, plus 700 MB per mod.
- A controller. The game reads one; the menus also take the keyboard.

The game opens in a 1280x720 window. It is not fullscreen by default and does
not take the screen.
