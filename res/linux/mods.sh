#!/bin/sh
# NBA JAM: On Fire Edition (OpenJam) - install, list and remove mods.
#
#   ./mods.sh install <path to the mod's .pkg> [name]
#   ./mods.sh list
#   ./mods.sh remove <id>
#   ./mods.sh use <id>          (the mod the game starts in)
#
# Needs Python 3, which almost every Linux system and the Steam Deck have.

here="$(cd "$(dirname "$0")" && pwd)" || exit 1
if ! command -v python3 >/dev/null 2>&1; then
    echo "Installing mods needs Python 3, and this system does not have it."
    exit 1
fi
if [ $# -eq 0 ]; then
    set -- list
fi
exec python3 "$here/setup/mods_cli.py" "$here" "$@"
