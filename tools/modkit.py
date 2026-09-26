#!/usr/bin/env python3
"""Installing a mod into a staged game, from the player's side.

tools/dlc.py is the workshop version of this: it works from the extracted
disc, a folder of imported mods and a build directory, and it rebuilds
everything every time. That is right for developing the port and wrong for
somebody who has the game folder and a mod they downloaded.

This does the same job with only what a player has:

    NBA JAM On Fire Edition PC/     <- the game folder, and the base game
        default.xex, data/          <- what a new mod is built on top of
        mods/<id>/                  <- one folder per mod, built here
        saves/<id>/                 <- kept apart, per mod
        mods.list                   <- every root lists all of them

Installing is: open the package, lay down a copy of the base game, write the
mod's files over it - converting the artwork on the way - give the front end
this port's mod list, and tell every root about the new one.

The copy is made of hard links, so a mod costs what its own files cost rather
than another four gigabytes.
"""

import glob
import os
import re
import shutil
import subprocess
import time
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ast_repack
import frontend
import roster_update

# CREATE_NO_WINDOW: keeps a console from flashing over the window.
NO_WINDOW = 0x08000000

HERE = Path(__file__).resolve().parent

# Things in the game folder that are not the game: they must not be copied
# into a mod's root, or each mod would carry a copy of every other one.
NOT_THE_GAME = {"mods", "saves", "logs", "ui", "metadata", "mods.list",
                "saves.path", "crash_stack.txt", "hooks.txt"}
NOT_THE_GAME_SUFFIX = (".exe", ".dll", ".log", ".pdb")

# PS3-only files, and the two archives that are platform code rather than
# data. Same list dlc.py imports with, and for the same reasons.
SKIP_SUFFIXES = (".gtf",)
SKIP_PATTERNS = (
    re.compile(r"^xenon/saveload/"),
    re.compile(r"^xenon/genbigs/rend_misc_big\.ast$"),
    re.compile(r"^xenon/bigs/audio/aemsdata\.ast$"),
)

SAVE_DIR = "saves"
SAVE_MARKER = "saves.path"
MODS_DIR = "mods"
MOD_LIST = "mods.list"
BASE = "base"
ABOUT = "mod.txt"


def slugify(text):
    s = re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")
    return s or "mod"


def find_container(folder):
    """The Xbox Live download in this folder, or None.

    What a player has before anything is unpacked is one file with no
    extension and a long hexadecimal name, the best part of a gigabyte. That
    is what this looks for; the game itself checks the header before it
    believes any of it.
    """
    folder = Path(folder)
    if not folder.is_dir():
        return None
    best = None
    for child in sorted(folder.iterdir()):
        if not child.is_file() or child.suffix:
            continue
        try:
            if child.stat().st_size < 40 * 1024 * 1024:
                continue
        except OSError:
            continue
        name = child.name
        if len(name) >= 16 and all(c in "0123456789abcdefABCDEF" for c in name):
            return child
        best = best or child
    return best


def unpack_container(game_exe, container, into, say=print, progress=None):
    """Get the game out of an Xbox Live download. Returns True when it worked.

    The reading is done by the game itself - it carries the container reader
    already, and there is no sense writing a second one here. It reports how
    far along it is in a file beside the game, which is what is watched.
    """
    note = Path(into) / "unpacking.txt"
    if note.exists():
        note.unlink()
    say("Unpacking your copy of the game. This takes a minute.")
    proc = subprocess.Popen(
        [str(game_exe), "--unpack", str(container), "--into", str(into)],
        creationflags=NO_WINDOW)
    while proc.poll() is None:
        time.sleep(0.25)
        try:
            done, total, name = note.read_text(encoding="utf-8").split(" ", 2)
            if progress:
                progress(int(done), int(total))
            say("Unpacking " + name.strip())
        except Exception:                            # noqa: BLE001
            pass
    if note.exists():
        note.unlink()
    if proc.returncode != 0 or not is_game_folder(into):
        return False
    say("Unpacked.")
    return True


def is_game_folder(path):
    path = Path(path)
    return (path / "default.xex").is_file() and (path / "data").is_dir()


def rel_files(root):
    for dirpath, _dirs, names in os.walk(root):
        for name in names:
            p = Path(dirpath) / name
            yield p, p.relative_to(root).as_posix()


def base_game_files(game):
    """Everything in the game folder that belongs to the game itself."""
    for child in sorted(Path(game).iterdir()):
        if child.name in NOT_THE_GAME:
            continue
        if child.is_file():
            if child.suffix.lower() in NOT_THE_GAME_SUFFIX:
                continue
            yield child, child.name
        elif child.is_dir():
            for p, rel in rel_files(child):
                yield p, child.name + "/" + rel


def link_or_copy(src, dst):
    try:
        os.link(src, dst)
    except OSError:
        shutil.copy2(src, dst)


def map_path(rel):
    """PS3 data path -> this build's data path."""
    return rel.replace("ps3/", "xenon/", 1) if rel.startswith("ps3/") else rel


def unpack_here(pkg, into, say):
    """A PS3 package opened into a folder. Returns the tree inside it.

    The extractor is imported and called rather than run as a program: a
    packaged build has no interpreter to hand.
    """
    sys.path.insert(0, str(HERE))
    import ps3_pkg_extract
    # These mods are published as a pair: "...-Part_1.pkg", a few hundred
    # bytes, and "...-Part_2.pkg", which is the whole mod. A PS3 installs
    # both, and a person picking one naturally picks the first. So whichever
    # was picked, every part beside it is opened into the same place.
    parts = [Path(pkg)]
    m = re.match(r"(.*)-Part_\d+\.pkg$", Path(pkg).name, re.IGNORECASE)
    if m:
        siblings = sorted(Path(pkg).parent.glob(glob.escape(m.group(1)) + "-Part_*.pkg"))
        if siblings:
            parts = siblings
    for part in parts:
        say("Opening %s" % part.name)
        ps3_pkg_extract.extract(str(part), str(into))
    usrdir = Path(into) / "USRDIR"
    return usrdir if usrdir.is_dir() else Path(into)


def read_about(root):
    """What a built mod says about itself, or nothing if it does not say."""
    out = {}
    note = Path(root) / ABOUT
    if note.is_file():
        for line in note.read_text(encoding="utf-8").splitlines():
            if "=" in line:
                k, v = line.split("=", 1)
                out[k.strip()] = v.strip()
    return out


def listed(game):
    """What the game's own mods.list says, keyed by id.

    A mod built by the workshop tool has no mod.txt of its own; its name and
    version live only in this file. Reading both means a game folder built
    either way shows the same thing.
    """
    out = {}
    path = Path(game) / MOD_LIST
    if not path.is_file():
        return out
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.startswith("mod "):
            continue
        field = line[4:].split("	")
        if len(field) < 3:
            continue
        out[field[0].strip()] = {
            "id": field[0].strip(), "path": field[1].strip(),
            "name": field[2].strip(),
            "version": field[3].strip() if len(field) > 3 else "",
            "author": field[4].strip() if len(field) > 4 else "",
            "files": field[5].strip() if len(field) > 5 else "",
        }
    return out


def installed(game):
    """The base game, then every mod, in the order they are listed."""
    game = Path(game)
    known = listed(game)
    out = [{"id": BASE, "name": "Base game", "path": str(game),
            "version": "", "author": "", "files": ""}]
    mods = game / MODS_DIR
    if mods.is_dir():
        for child in sorted(mods.iterdir()):
            if not child.is_dir() or not is_game_folder(child):
                continue
            about = dict(known.get(child.name, {}))
            about.update({k: v for k, v in read_about(child).items() if v})
            about["id"] = child.name
            about["path"] = str(child)
            about.setdefault("name", child.name)
            out.append(about)
    return out


def active_id(game):
    """Which root the game is set to start in, from its own list."""
    listing = Path(game) / MOD_LIST
    if listing.is_file():
        for line in listing.read_text(encoding="utf-8").splitlines():
            if line.startswith("active "):
                return line.split(None, 1)[1].strip()
    return BASE


def write_mod_list(game, mods, active, into):
    lines = ["# Written by the mod manager. LT+Y on the main menu opens the "
             "chooser.",
             "# mod <id> <TAB> <root> <TAB> <name> <TAB> <version> <TAB> "
             "<author> <TAB> <files>",
             "active {}".format(active)]
    for m in mods:
        lines.append("mod {}\t{}\t{}\t{}\t{}\t{}".format(
            m["id"], m["path"], m.get("name", m["id"]), m.get("version", ""),
            m.get("author", ""), m.get("files", "")))
    (Path(into) / MOD_LIST).write_text("\n".join(lines) + "\n",
                                       encoding="utf-8")


def refresh_lists(game):
    """Tell every root about every other one."""
    game = Path(game)
    mods = installed(game)
    active = active_id(game)
    if active not in {m["id"] for m in mods}:
        active = BASE
    for m in mods:
        write_mod_list(game, mods, active, m["path"])
    return mods


def refresh_menus(game, say=lambda _s: None):
    """Redraw every root's mod list, because the list itself changed."""
    game = Path(game)
    mods = installed(game)
    listed = [m for m in mods if m["id"] != BASE]
    for m in mods:
        try:
            frontend.apply(m["path"], listed,
                           None if m["id"] == BASE else m["id"])
        except SystemExit as exc:            # not a game root; leave it
            say("  skipped %s (%s)" % (m["id"], exc))
        except Exception as exc:
            say("  could not redraw %s: %s" % (m["id"], exc))


def install(game, source, name=None, version="", author="", say=print,
            progress=lambda _done, _total: None):
    """Put a mod into the game folder. `source` is a .pkg or a folder."""
    game = Path(game)
    if not is_game_folder(game):
        raise RuntimeError("%s is not the game folder" % game)
    source = Path(source)

    temp = None
    try:
        if source.is_file() and source.suffix.lower() == ".pkg":
            temp = Path(tempfile.mkdtemp(prefix="nbajam_pkg_"))
            source = unpack_here(source, temp, say)
        data = source / "data" if (source / "data").is_dir() else source
        if not (data / "ps3").is_dir() and not (data / "xenon").is_dir():
            raise RuntimeError("no game data inside that - expected a PS3 mod")

        name = name or source.parent.name
        mod_id = slugify(name)
        root = game / MODS_DIR / mod_id
        if root.exists():
            shutil.rmtree(root)
        root.mkdir(parents=True)

        # What the mod brings, mapped onto this build's names.
        mod_files = {}
        for path, rel in rel_files(data):
            mapped = map_path(rel)
            if mapped.endswith(SKIP_SUFFIXES) or any(
                    p.match(mapped) for p in SKIP_PATTERNS):
                continue
            mod_files["data/" + mapped] = path

        base = list(base_game_files(game))
        total = len(base) + len(mod_files)
        say("Building %s from %d game files and %d mod files"
            % (mod_id, len(base), len(mod_files)))

        done = 0
        replaced = added = converted = 0
        for path, rel in base:
            target = root / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            if rel in mod_files:
                src = mod_files.pop(rel)
                if rel.lower().endswith(".ast") and _convert(src, target, path):
                    converted += 1
                else:
                    shutil.copy2(src, target)
                replaced += 1
            else:
                link_or_copy(path, target)
            done += 1
            progress(done, total)
        for rel, src in mod_files.items():           # files the game lacks
            target = root / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, target)
            added += 1
            done += 1
            progress(done, total)

        say("  %d files replaced, %d added, %d archives converted"
            % (replaced, added, converted))

        # EA's last roster update, which every published mod carries because
        # every published mod's rosters name the players in it. It sits
        # beside the mod's data rather than inside it, so the loop above did
        # not see it. Converted and kept beside the game's data, which is
        # where the game looks for it. See tools/roster_update.py.
        update = source / roster_update.NAME
        if update.is_file():
            try:
                n = roster_update.install(str(update), str(root), quiet=True)
                say("  the roster update came too: %d texture(s) converted" % n)
            except Exception as exc:                 # noqa: BLE001
                say("  the bundled roster update would not convert: %s" % exc)

        # The PS3 audio config asks for data/ps3 by name.
        alias, xenon = root / "data" / "ps3", root / "data" / "xenon"
        if xenon.is_dir() and not alias.exists():
            try:
                # CREATE_NO_WINDOW: without it this flashes a black console
                # over the window every time a mod is installed.
                subprocess.run(["cmd", "/c", "mklink", "/J", str(alias),
                                str(xenon)], check=True, capture_output=True,
                               creationflags=NO_WINDOW)
            except Exception:
                for p, rel in rel_files(xenon):
                    t = alias / rel
                    t.parent.mkdir(parents=True, exist_ok=True)
                    link_or_copy(p, t)

        (root / ABOUT).write_text(
            "id = {}\nname = {}\nversion = {}\nauthor = {}\nfiles = {}\n"
            .format(mod_id, name, version, author, replaced + added),
            encoding="utf-8")

        # Saves live with the game, one folder per mod, so removing a mod
        # cannot take anyone's progress with it.
        (game / SAVE_DIR / mod_id).mkdir(parents=True, exist_ok=True)
        (root / SAVE_MARKER).write_text(
            "../../{}/{}\n".format(SAVE_DIR, mod_id), encoding="utf-8")

        say("Redrawing the menus")
        refresh_menus(game, say)
        refresh_lists(game)
        say("Installed %s" % name)
        return mod_id
    finally:
        if temp and temp.exists():
            shutil.rmtree(temp, ignore_errors=True)


def _convert(src, target, stock):
    """Convert one archive's artwork, keeping the disc's own answers."""
    try:
        return ast_repack.repack(str(src), str(target), stock=stock, quiet=True)
    except Exception:
        if Path(target).exists():
            Path(target).unlink()
        return False


def remove(game, mod_id, keep_saves=True, say=print):
    """Take a mod out again. Its saves stay unless asked otherwise."""
    game = Path(game)
    root = game / MODS_DIR / mod_id
    if not root.is_dir():
        raise RuntimeError("no mod called %s" % mod_id)
    shutil.rmtree(root)
    if not keep_saves:
        shutil.rmtree(game / SAVE_DIR / mod_id, ignore_errors=True)
    say("Removed %s" % mod_id)
    refresh_menus(game, say)
    refresh_lists(game)


def set_active(game, mod_id):
    """Which root the game starts in next time."""
    game = Path(game)
    mods = installed(game)
    if mod_id not in {m["id"] for m in mods}:
        raise RuntimeError("no mod called %s" % mod_id)
    for m in mods:
        write_mod_list(game, mods, mod_id, m["path"])


def play(game):
    """Start whichever root is active."""
    game = Path(game)
    mods = {m["id"]: m for m in installed(game)}
    root = mods.get(active_id(game), mods[BASE])["path"]
    exe = game / "nbajam_ofe.exe"
    subprocess.Popen([str(exe), '--game_data_root=%s' % root], cwd=root,
                     creationflags=NO_WINDOW)
