#!/usr/bin/env python3
"""Install, list and remove mods from a terminal. What the Mod Manager's
window does, for machines where that window does not run (Linux, so far).

    python3 mods_cli.py <game folder> list
    python3 mods_cli.py <game folder> install <mod.pkg> [name]
    python3 mods_cli.py <game folder> remove <id>
    python3 mods_cli.py <game folder> use <id>

`use` picks the mod the game starts in; inside the game, LT+Y on the main
menu switches between them as well. Only the standard library is needed:
the PS3 packages are opened with the system's OpenSSL when pycryptodome is
not installed.
"""

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import modkit  # noqa: E402


def say(text):
    print(text, flush=True)


def show(game):
    active = modkit.active_id(game)
    for m in modkit.installed(game):
        mark = "  (starts in this one)" if m["id"] == active else ""
        print("  %-36s %s%s" % (m["id"], m.get("name", m["id"]), mark))


def main(argv):
    if len(argv) < 2 or argv[1] not in ("list", "install", "remove", "use"):
        print(__doc__)
        return 2
    game = Path(argv[0])
    if not modkit.is_game_folder(game):
        print("The game is not unpacked in %s yet: run nbajam.sh once first."
              % game)
        return 1
    modkit.stage_ui(game)
    command, rest = argv[1], argv[2:]

    if command == "list":
        show(game)
        return 0

    if command == "install":
        if not rest:
            print("Which mod? Give the path to its .pkg file.")
            return 2
        package = Path(rest[0])
        if not package.exists():
            print("There is nothing at %s" % package)
            return 1
        name = " ".join(rest[1:]) or None
        if name is None and package.suffix.lower() == ".pkg":
            # The name the manager suggests: the file's, without the part
            # number and the underscores.
            stem = package.stem
            for cut in ("-Part_1", "-Part_2", "-Part_3"):
                stem = stem.replace(cut, "")
            name = stem.replace("_", " ").replace("-", " ").strip()
            # "... Edition v6.0 by johnz1": the version and author are not
            # part of the name a player picks from.
            name = re.sub(r"\s+v\d[\d.]*(\s+by\s+.*)?$", "", name,
                          flags=re.IGNORECASE) or name
        last = [-1]

        def progress(done, total):
            pct = done * 100 // max(1, total)
            if pct // 10 != last[0]:
                last[0] = pct // 10
                print("  %d%%" % pct, flush=True)

        modkit.install(game, package, name=name, say=say, progress=progress)
        print("\nInstalled. Your mods:")
        show(game)
        return 0

    if not rest:
        print("Which mod? These are installed:")
        show(game)
        return 2
    if command == "remove":
        modkit.remove(game, rest[0], keep_saves=True, say=say)
    else:
        modkit.set_active(game, rest[0])
        print("The game will start in %s." % rest[0])
    show(game)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except RuntimeError as exc:
        print(exc)
        sys.exit(1)
