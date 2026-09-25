#!/usr/bin/env python3
"""Build the mod manager into one executable, and give it the game's icon.

    python tools/build_manager.py

Leaves `NBA JAM Mod Manager.exe` in the staged game folder, where it sits
beside the game and manages the mods around it. `tools/dlc.py stage` copies
it in as well, so a rebuild of the game folder keeps it.

The icon is the game's own logo, which lives as a texture in the front end -
the same untiling the mod importer does, then straight into an .ico.
"""

import io
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
PORT = HERE.parent
PARENT = PORT.parent
GAME_ROOT = PARENT / "Root"
STAGE = PARENT / "NBA JAM On Fire Edition PC"
LOGO_AST = "data/xenon/fe/bounce/parts/jamlogosmall.ast"
NAME = "NBA JAM Mod Manager"


def make_icon(out):
    """The logo, out of the front end and into an .ico."""
    sys.path.insert(0, str(HERE))
    import ast_textures as T
    from PIL import Image

    src = GAME_ROOT / LOGO_AST
    if not src.is_file():
        print("no logo archive at %s; building without an icon" % src)
        return None
    archive = T.Ast(str(src))
    for entry in archive.entries:
        try:
            blob = archive.read(entry)
        except Exception:
            continue
        if T.kind(blob) != "xpr2":
            continue
        dds, _w, _h = T.xpr2_to_dds(blob)
        image = Image.open(io.BytesIO(dds)).convert("RGBA")
        # The logo sits in the middle of a square with a lot of air around it;
        # trimming to what is actually drawn makes it readable at 16 pixels.
        box = image.getbbox()
        if box:
            image = image.crop(box)
        side = max(image.size)
        square = Image.new("RGBA", (side, side), (0, 0, 0, 0))
        square.alpha_composite(image, ((side - image.width) // 2,
                                       (side - image.height) // 2))
        square.save(out, sizes=[(256, 256), (128, 128), (64, 64), (48, 48),
                                (32, 32), (16, 16)])
        print("icon written to %s" % out)
        return out
    return None


def main():
    build = PORT / "out" / "manager"
    build.mkdir(parents=True, exist_ok=True)
    icon = make_icon(build / "manager.ico")

    argv = [sys.executable, "-m", "PyInstaller", "--noconfirm", "--onefile",
            "--windowed", "--name", NAME,
            "--distpath", str(build / "dist"),
            "--workpath", str(build / "work"),
            "--specpath", str(build),
            # Read at runtime from the files rather than imported by name.
            "--hidden-import", "PIL.Image",
            "--hidden-import", "Crypto.Cipher.AES",
            str(HERE / "modmanager.py")]
    if icon:
        argv[argv.index("--windowed") + 1:argv.index("--windowed") + 1] = [
            "--icon", str(icon)]
    print("building...")
    result = subprocess.run(argv, cwd=str(PORT))
    if result.returncode:
        return result.returncode

    exe = build / "dist" / (NAME + ".exe")
    if not exe.is_file():
        print("no executable came out of that")
        return 1
    size = exe.stat().st_size / (1024 * 1024)
    print("built %s (%.0f MB)" % (exe, size))
    if STAGE.is_dir():
        shutil.copy2(exe, STAGE / exe.name)
        print("copied into %s" % STAGE)
    return 0


if __name__ == "__main__":
    sys.exit(main())
