#!/bin/sh
# NBA JAM: On Fire Edition (OpenJam) - start the game, setting it up first if
# this is the first run. Everything stays in this folder.

here="$(cd "$(dirname "$0")" && pwd)" || exit 1
cd "$here" || exit 1

say() {
    printf '%s\n' "$*"
    # Started from a file manager there is no terminal to read; say it on
    # the desktop as well when there is a way to.
    if [ ! -t 1 ] && command -v notify-send >/dev/null 2>&1; then
        notify-send "NBA JAM" "$*" 2>/dev/null
    fi
}

if [ ! -f default.xex ] || [ ! -d data ]; then
    # The Xbox Live download: one file of the best part of a gigabyte with
    # a long hexadecimal name and no extension.
    container=""
    for f in *; do
        [ -f "$f" ] || continue
        if printf '%s' "$f" | grep -Eq '^[0-9A-Fa-f]{16,}$'; then
            container="$f"
            break
        fi
    done
    if [ -z "$container" ]; then
        say "Put your copy of NBA JAM: On Fire Edition in this folder first:" \
            "the one file from your console's drive, about 850 MB, with a" \
            "long name of numbers and letters and no extension. Then run" \
            "this again."
        exit 1
    fi
    say "Unpacking your copy of the game ($container). This takes a minute."
    if ! ./nbajam_ofe --unpack "$container" --into "$here"; then
        say "That file would not unpack. It should be the one from your" \
            "console's drive, about 850 MB, with no file extension."
        exit 1
    fi
    rm -f unpacking.txt
    # The typefaces and button pictures the port's own screens use. Optional:
    # without Python and Pillow the game draws those screens in a plain face.
    if command -v python3 >/dev/null 2>&1; then
        python3 -c 'import sys; sys.path.insert(0, "setup")
from pathlib import Path
import dlc
dlc.stage_fonts(Path("."))
dlc.stage_glyphs(Path("."))' >/dev/null 2>&1 || true
    fi
    say "Unpacked. Starting the game."
fi

# Start in the mod chosen last (./mods.sh use, or the game's own chooser),
# as the Mod Manager's Play button does on Windows.
if [ -f mods.list ]; then
    active="$(sed -n 's/^active //p' mods.list | head -n 1 | tr -d '\r')"
    if [ -n "$active" ] && [ "$active" != base ]; then
        root="$(awk -F '\t' -v want="mod $active" \
            '$1 == want { print $2; exit }' mods.list)"
        if [ -n "$root" ] && [ -d "$root" ]; then
            cd "$root" || exit 1
            exec "$here/nbajam_ofe" "--game_data_root=$root" "$@"
        fi
    fi
fi

exec ./nbajam_ofe "$@"
