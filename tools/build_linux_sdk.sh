#!/usr/bin/env bash
# All build products and caches stay in this checkout. Requires Clang, CMake,
# Ninja, Git and the SDL/X11/Wayland development dependencies already installed.
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$repo"
export TMPDIR="$repo/.local/linux/tmp"
export XDG_CACHE_HOME="$repo/.local/linux/cache"
export XDG_CONFIG_HOME="$repo/.local/linux/config"
export XDG_DATA_HOME="$repo/.local/linux/data"
export CCACHE_DIR="$repo/.local/linux/ccache"
export PYTHONDONTWRITEBYTECODE=1
mkdir -p "$TMPDIR" "$XDG_CACHE_HOME" "$XDG_CONFIG_HOME" "$XDG_DATA_HOME"
source_dir="$repo/.local/deps/rexglue-sdk"
build_dir="$repo/.local/sdk-linux-build"
prefix="$repo/.local/sdk-linux-built"
link_flags=""
if [ -n "${OPENJAM_LINUX_LIBRARY_PATH:-}" ]; then
    library_path=$(realpath "$OPENJAM_LINUX_LIBRARY_PATH")
    case "$library_path" in "$repo"/*) ;; *) echo 'Link libraries must be inside this repository' >&2; exit 1 ;; esac
    link_flags="-L$library_path"
fi
if [ ! -d "$source_dir/.git" ]; then
    git clone --depth 1 --branch v0.10.0 https://github.com/rexglue/rexglue-sdk.git "$source_dir"
fi
revision=$(git -C "$source_dir" rev-parse HEAD)
if [ "$revision" != f5337cdc947ff6d4c4196737e2c807a48f2a1fc2 ]; then
    printf 'Expected ReXGlue v0.10.0; found %s\n' "$revision" >&2
    exit 1
fi
git -C "$source_dir" submodule update --init --recursive --depth 1
for patch in "$repo"/tools/sdk-patches/*.patch; do
    if git -C "$source_dir" apply --reverse --check "$patch" 2>/dev/null; then
        continue
    fi
    git -C "$source_dir" apply --check "$patch"
    git -C "$source_dir" apply "$patch"
done
cmake -S "$source_dir" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2 \
    "-DCMAKE_SHARED_LINKER_FLAGS=$link_flags" "-DCMAKE_EXE_LINKER_FLAGS=$link_flags" \
    -DREXGLUE_USE_VULKAN=ON -DREXGLUE_USE_D3D12=OFF \
    -DREXGLUE_REGISTER_USER_PACKAGE=OFF -DCMAKE_EXPORT_PACKAGE_REGISTRY=OFF \
    -DSDL_X11_XSCRNSAVER=OFF -DSDL_X11_XTEST=OFF \
    -DCMAKE_INSTALL_PREFIX="$prefix"
cmake --build "$build_dir" -j "${1:-6}"
cmake --install "$build_dir"
