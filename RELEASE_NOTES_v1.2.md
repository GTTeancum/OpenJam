# OpenJam v1.2

Higher render resolutions, portable saves and broader controller support.

**Linux: 2× render scale is recommended. 3× is experimental and may crash.**
See the validation notes below for the Linux test results and known limits.

## Changes

- **Vulkan is now the default on Windows and Linux.** Windows builds include
  the patched Vulkan renderer used on Linux. This avoids the D3D12 match-loading
  failure seen at higher render scales. D3D12 remains an optional backend.
- **Video settings separate output resolution from render scale.** Output
  resolution goes up to 3840×2160; 1×/2×/3× render scale remains available.
  At 16:9, 2× renders 2560×1440 and 3× renders 3840×2160. Restart after changing
  either setting. Addresses #8, #9, #11 and the tested base-game case in #17.
- **Saves live beside the game:** `saves/base/` for the original game,
  `saves/<mod>/` for each mod, and `cache/` for shaders. No `portable.txt` needed.
  On the first base-game launch with no local progress, legacy saves are copied
  and verified automatically. Originals are retained and existing local progress
  is never overwritten. Fixes #18.
- **The mouse cursor stays hidden in the game window.** Fixes #10.
- **The mod chooser uses SDL for controllers on both platforms**, including
  DualSense Edge mappings. Hold LT + Y (L2 + Triangle), navigate with the d-pad,
  then A/Cross to load or B/Circle to close. Any connected controller can open
  it; that controller owns the chooser until it closes. Disconnecting closes
  it safely, and reconnecting requires no restart. Fixes #14.
- **Download verification:** each archive includes `SHA256SUMS.txt`. Matching
  `.sha256` archive hashes and `.files.sha256` file lists are attached to this
  release, including hashes for the ReXGlue profiler client discussed in #15.

## Upgrading and saves

Extract the new release into a writable folder and set it up with your own
XBLA game container. Reinstall mods from their original packages. If your old
installation already has a `saves/` folder, copy that entire folder into the
new installation before playing.

Automatic legacy import looks in `Documents/nbajam_ofe` on Windows (including
redirected Documents) and `$XDG_DATA_HOME/nbajam_ofe` on Linux, normally
`~/.local/share/nbajam_ofe`. It imports this game's save packages and Headers,
profile and achievements into an empty `saves/base/`. It does not import caches
or put base-game progress into a mod. Keep the originals until you confirm your
progress in the game.

For a manual import, close the game and back up both locations, then copy the
legacy folder's contents except `cache/` into an empty `saves/base/`. Preserve
the player-ID folders and Headers.

## Validation and known limits

The native Linux build requires an x86-64-v2 CPU, Vulkan, glibc 2.38 or newer,
and libstdc++ with GLIBCXX_3.4.32 (GCC 13.2 or newer), as provided by Ubuntu
24.04. Its ELF requirements are checked during packaging.

- Windows: native Vulkan base-game match captures at 2× and 3×, including
  3840×2160 output and repeat launch with existing saves/cache, on AMD Radeon
  780M. Court, players, crowd, scoreboard and turbo bars were inspected.
- Linux: Ubuntu 26.04.1 / WSL2, Mesa llvmpipe 26.0.8 software Vulkan. The packaged
  launcher unpacked all 536 game files, with the XEX matching the original.
  Default 1× reached a correctly rendered base-game match at 1280×720; court,
  players, crowd, scoreboard and turbo bars were inspected. The 2× repeat run
  also rendered gameplay at 2560×1440, but logged memory-exhaustion traps.
  Initial software shader compilation required a longer launch window.
- Linux save migration: an actual first-run import restored four game-created
  save/header files into `saves/base/`, leaving the legacy originals unchanged.
  The standalone migration and SDL controller regression tests also passed.
- The SDL controller harness exercises selection of a second pad, chooser
  ownership, navigation/load/cancel, guest-input gating and reconnect using
  virtual pads confined to the test process. Physical DualSense Edge USB and
  Bluetooth still need hardware testing.
- These checks do not establish full-match stability, every mod, turbo-bar
  depletion/recovery, audible audio or performance across GPU vendors.
- D3D12 still has a known match-loading failure at higher render scales;
  Vulkan is the release default. `--renderer=d3d12` remains available for
  troubleshooting on Windows.
- **Linux 3× is experimental and may crash; 2× is recommended.**
  Mesa llvmpipe 26.0.8 cannot start at 3×.
  The renderer needs a 4.5 GiB virtual buffer, beyond this driver's buffer
  limit of just under 4 GiB. Linux 3× has not been
  validated on a hardware Vulkan driver. This is a buffer-address-space limit,
  not a statement that 4.5 GiB of physical VRAM must always be resident.
- **Unresolved Linux 2× diagnostics:** multiple runs attempted to write
  `MemoryExhausted.xml` and hit guest CPU traps. A follow-up with writes enabled
  in the private test copy aborted on an invalid guest function call at address
  zero before producing that report. The root cause is not established, and
  the successful gameplay capture does not make this a clean validation pass.

The downloads contain code and achievement definitions only. Game data, mods,
extracted artwork and player saves are not included.
