#!/usr/bin/env bash
# Link against Ubuntu 24.04's libm when building on newer glibc hosts. The
# library is a link-time input only; the release uses the player's system libm.
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$repo"
mkdir -p .local/downloads .local/linux-compat .local/linux/tmp
export TMPDIR="$repo/.local/linux/tmp"
package=libc6_2.39-0ubuntu8.12_amd64.deb
archive="$repo/.local/downloads/$package"
if [ ! -f "$archive" ]; then
    curl --fail --location --output "$archive" \
        "https://archive.ubuntu.com/ubuntu/pool/main/g/glibc/$package"
fi
printf '%s  %s\n' d821b41fc59cb4978ecd53bc83e5c964ac15d305c8d2ac4c8a35aebe55010fb9 "$archive" | sha256sum -c -
# Extract exactly one regular file; do not install the package or follow any
# of its filesystem links. Its SONAME remains libm.so.6.
dpkg-deb --fsys-tarfile "$archive" | \
    tar -xOf - ./usr/lib/x86_64-linux-gnu/libm.so.6 > .local/linux-compat/libm.so
printf 'Link library directory: %s/.local/linux-compat\n' "$repo"
