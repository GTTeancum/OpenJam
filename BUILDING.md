# NBA JAM: On Fire Edition — native PC port (ReXGlue)

Static recompilation of the Xbox 360 XBLA release into a native Windows
executable. No emulator: the PowerPC code is translated to C++ ahead of time and
compiled for x86-64.

- **Title ID:** 584111EC
- **Original binary:** `nbajamzf.exe`, built 2011-08-16
- **Image:** base `0x82000000`, size `0xC50000` (12.9 MB), entry `0x82246498`
- **XEX format:** encrypted (retail key) + LZX "normal" compression, 32 KB window

## Layout

| Path | What it is |
| --- | --- |
| `D:\Programming\GitHub\NBA JAM On Fire Edition\Root` | Extracted XBLA package. Game data root. |
| `D:\Programming\GitHub\NBA JAM On Fire Edition\port` | This project. |
| `D:\Programming\GitHub\rexglue-dist\win-amd64` | ReXGlue SDK v0.10.0, prebuilt. Consumed via `CMAKE_PREFIX_PATH`. |
| `D:\Programming\GitHub\rexglue-sdk` | ReXGlue SDK source at tag v0.10.0. Reference only — not built. |

The 892 MB `B3B890C25C6BBFA71CF19CEC1A037410A2265FEC58` file in the parent folder
is the original signed XBLA package. It is not used by the build; everything
comes from the extracted `Root` folder.

## Toolchain

Everything below was already installed on this machine. Nothing was added.

- Visual Studio 2022 Community 17.14 (for the MSVC STL and Windows SDK)
- Clang 22.1.2 — `C:\Program Files\LLVM`
- Ninja — bundled inside Visual Studio, not on PATH
- CMake 4.3.1

Absolute paths for all of these are pinned in `CMakeUserPresets.json`, which is
machine-local and deliberately not part of the committed preset set. If you move
the SDK or upgrade LLVM, that is the one file to edit.

## Build

```
cmake --preset local-relwithdebinfo
cmake --build --preset local-relwithdebinfo -j 10
```

Codegen runs automatically as a build step; it re-runs only when the manifest,
the override config, or the XEX changes. To run it alone:

```
cmake --build --preset local-relwithdebinfo --target nbajam_ofe_codegen
```

Expect roughly 3 minutes of codegen and 10 minutes of compilation from clean.
Codegen emits about 5.7 million lines of C++ across 158 translation units
(166 MB), covering 52,737 functions.

`-j 10` rather than `-j 16` is a precaution, not a measured limit: this
machine has 16 threads and 28 GB, and the generated translation units are
large. `-j 16` was never tried, so it may well be fine.

## Running

The executable needs the game data mounted as the guest's `game:` / `d:` device.
Pass it explicitly:

```
nbajam_ofe.exe --game_data_root="D:\Programming\GitHub\NBA JAM On Fire Edition\Root"
```

The wiki says this defaults to `argv[1]` or to an `assets` folder beside the
executable. Neither fallback fires in v0.10.0 — launching with no arguments
exits immediately, and `logs/nbajam_ofe_NNN.log` says only:

```
--game_data_root was not provided.
```

An `assets` junction pointing at `Root` exists in the build folder and is
harmless, but it is not what makes the game start. The flag is.

`run.ps1` wraps both flags and tails the log afterwards:

```
.\run.ps1
.\run.ps1 -Config local-debug --log_level=debug
```

`--metadata_root` is needed for achievements. The runtime looks for
`achievements.toml` under the *game data* root, not the project, so without the
flag the 12 extracted achievements are simply not loaded.

In-app: **F3** debug overlay, **F4** settings/CVar editor, **backtick** console.

## Local changes and why they exist

### `nbajam_ofe_config.toml` — two manual function declarations

Function discovery left two branch targets uncovered, which codegen turns into
`REX_FATAL` calls that abort the process if reached:

```
b 0x822AE5C8 from 0x822B2D74
b 0x825EE020 from 0x825EE0EC
```

Both call sites are C++ multiple-inheritance adjustor thunks — two instructions
that subtract a fixed offset from `this` and tail-call the real virtual method.
The methods are only ever entered through a vtable slot or that thunk, so
nothing in the image references their addresses directly and discovery left them
in gaps.

Their extents were determined by counting instructions in the neighbouring
recompiled functions rather than guessed:

- `sub_822AE5B0` is six instructions (0x18 bytes) and ends exactly at
  `0x822AE5C8`. The next discovered function starts at `0x822AE5F8`.
- `sub_825EE018` is a two-instruction thunk ending exactly at `0x825EE020`.
  The next discovered function starts at `0x825EE038`.

### `nbajam_ofe_gaps.toml` — 520 recovered gap functions

This is the substantial one, and the part most likely to need extending.

Function discovery reaches code through direct calls and a vtable scanner.
Anything referenced only from a data table the scanner does not recognise is
never identified as a function, and the bytes sit in a gap between two
discovered functions. Nothing goes wrong at build time. It goes wrong the first
time the game makes an indirect call to one of those addresses:

```
[FATAL] Call to invalid or unregistered function at guest address 0x82B083E0
```

That was the first-launch failure. Fixing them one crash at a time costs a full
codegen-and-build cycle each, so they were recovered in bulk instead.

**How the extents were derived.** The generated C++ emits one `// mnemonic`
comment per guest instruction, so a function's length is
`4 * (instruction count)`. `generated/default/nbajam_ofe_register.cpp` maps every
address to its function name. Together those give the exact extent of all 52,172
recompiled functions, and every run of bytes no function covers is a gap.

Two exclusions:

- **4-byte gaps.** There are 27,614 of them and 27,612 sit at `end % 8 == 4` —
  alignment padding to the 8-byte function alignment this image uses, not code.
- **The 6,932-byte run at 0x82B17E1C**, which abuts the end of `.text` and is
  tail data.

That leaves 407 candidates totalling 20 KB.

**How they were validated.** There is no disassembler here — the XEX is
encrypted and LZX-compressed, so the bytes cannot be read directly without
reimplementing the loader. Codegen itself is the disassembler: declare the
candidates, run it, and read its verdict. Of the 407:

- 43 could not be decoded into basic blocks (`has no blocks — generating stub`).
  Padding or data. Dropped.
- 8 produced branches to addresses outside the declared extent, meaning the
  declared bounds cut a real function. Dropped.
- 356 decoded cleanly.

Those rejects are recorded permanently in `tools/gap_banlist.txt` so no later
pass re-proposes them.

**One pass is not enough.** `end` is only an upper bound: codegen stops at the
real end of the first function it finds, so a gap holding a run of several
functions yields exactly one per pass. The image has tables of near-identical
0x18-byte wrappers, so these runs are common. `tools/recover_gaps.py` drives the
loop — measure, declare, run codegen, drop what it rejects, repeat — and is
resumable, since all its state lives in the TOML and the ban list:

```
python tools/recover_gaps.py
```

The loop runs to convergence and its declaration count moves as the
fragment filter prunes and the loop re-fills; treat the number in the
heading as current rather than fixed. The final codegen reports zero errors, zero warnings and
zero unresolved calls, and the runtime registers all 53,044 functions with none
rejected.

**If a new unregistered-function crash appears**, it is another gap this pass
did not recover — most likely one of the 51 dropped entries, or inside the tail
data. Add it to this file with an `end` that stops at the next known function,
re-run codegen, and check the log is still clean before building. A declaration
that splits a real function shows up immediately as an unresolved branch, so
codegen will tell you if the bounds are wrong.

Do not simply re-add all 407. The dropped ones were dropped for cause, and a bad
declaration is worse than a missing one: a missing function crashes loudly at a
known address, while a wrongly-bounded one silently truncates working code.

### `src/kernel_usbcam_stubs.cpp` — Xbox LIVE Vision camera stubs

The game imports `XUsbcamGetState` and `XUsbcamSetConfig`. ReXGlue v0.10.0 has
an implementation of these at `src/kernel/xboxkrnl/xboxkrnl_usbcam.cpp`, but the
file is commented out of the SDK's kernel build:

```
# xboxkrnl/xboxkrnl_usbcam.cpp  # TODO: lol eventually.
```

so the symbols are absent from the shipped runtime and the link fails. This file
supplies them locally with the same behaviour as the SDK source: report no
camera connected. Delete it if a future SDK release enables that file.

### `CMakeLists.txt` — `GPU_PLUGINS xenos`

`rexglue init` generates `rexglue_setup_target(nbajam_ofe)` with no GPU plugin.
The Xenos graphics backend is loaded at runtime rather than linked, so nothing
stages `rexgpu-xenos*.dll` beside the executable and the game starts with no
graphics backend. The call needs to be:

```cmake
rexglue_setup_target(nbajam_ofe GPU_PLUGINS xenos)
```

This is a gap in the project template, not something you did wrong. Watch for it
if you ever re-run `rexglue init`.

### `src/crash_report.cpp` — guest call stacks on faults

The runtime reports an unhandled guest access violation as one line naming the
address that was touched, which does not say what touched it. There is no cdb or
windbg on this machine, and the LLVM `lldb.exe` here fails to start because it
wants `python311.dll` and only 3.12 is installed.

So this file installs a vectored exception handler that walks the native stack
and resolves it against the executable's own PDB. Recompiled guest functions are
ordinary C++ functions named `sub_<guest address>`, so the output reads as a
guest call stack and maps straight back into the XEX. It writes
`crash_stack.txt` beside the executable.

It is deliberately inert: it only looks at access violations, reports each
distinct faulting address once, caps itself at 12 reports, and always returns
`EXCEPTION_CONTINUE_SEARCH`. It observes and never changes behaviour, so
removing it cannot fix or break anything. Turn it off with
`-DREXPORT_CRASH_REPORT=OFF`.

For guest *register* state, prefer a ReXGlue `[[midasm_hook]]` — this handler
sees host frames, not PPC registers.

### `src/nbajam_ofe_app.h` — full version rather than trial

XBLA titles decide trial-versus-owned from `XamContentGetLicenseMask`, where
each bit is a granted license and bit 0 conventionally means purchased. ReXGlue
returns whatever the `license_mask` CVar holds, and that **defaults to 0** — so
every recompiled XBLA title comes up as a trial out of the box. NBA JAM does
exactly that; it ships a whole `data/xenon/fe/bounce/screens/trial` tree.

The extracted package carries a LIVE signature with License 0 granting bit 0, so
1 is what a real console reports for an owned copy. `OnPreSetup` fills that in.

Two details worth knowing:

- It only fills in a mask that is still zero. The config file and the command
  line are both applied in `SetupEnvironment`, *before* this hook runs in
  `SetupPresentation`, so setting it unconditionally would silently override
  whatever was asked for. Guarding on zero keeps `--license_mask=3` working.
- It cannot preserve an explicit `--license_mask=0`, which is indistinguishable
  from the default. To see the trial-only screens, comment out the assignment.

The resulting mask is logged at boot, because the game does not query it until
well into the front end and the log is otherwise the only way to confirm it
without playing:

```
[info] [core] license_mask = 1 (full version)
```

### `metadata/`

12 achievements and their icons, extracted from the XEX with
`rexglue init achievements`. CMake embeds `metadata/icons` into the executable
automatically when the folder exists.

## The startup bug, and what fixed it

**Cause: declarations in this project that were function *fragments*, not
functions.**

`tools/recover_gaps.py` declares a function at the start of every uncovered run
of bytes. Most are genuine functions discovery missed. Thirty-seven were not:
they were pieces of functions the analyzer had already covered - bare epilogues,
bodies with no terminator at all, and fragments that write through r31 without
ever setting it. The smallest is the whole story:

```
sub_82579D38:
    ld   r31,-16(r1)
    blr
```

Declaring one is worse than leaving the gap alone. The address is registered in
the guest function table, so anything dispatching to it restores registers from
stack slots never written for the live frame. Those slots hold `0xBE`, the byte
`XThread::AllocateStack` paints new stacks with. Two of the fragments wrote
through an inherited r31 at offsets +128 and +136, which lands exactly inside
the register-save window of a 176-byte frame.

That is fatal because `sub_82558718` keeps its frame pointer in r31 and restores
the stack pointer from it (`addi r1,r31,176`). With r31 poisoned the guest stack
pointer is destroyed, and everything after is downstream: every stack-relative
address is garbage but lands in committed physical memory so nothing faults,
`NtReadFile` refuses the resulting buffer, `memory.cfg` never loads, the memory
framework asserts `Don't recognise category named '%s'`, and the assert's own
argument faults at `0x68EC0000`. That last step was the only line the log showed.

### How it was found

The guest stack pointer was measured at the entry point (`0x70170000`, healthy)
and then at every call, narrowing over six rounds to `sub_82558718`, using
`tools/probe_r1.py`, `tools/probe_sites.py` and a guard added to the
indirect-call dispatcher. Whole-image scans then found every function with the
defect: `tools/check_nonvolatile.py` (writes a callee-saved register without
saving it) and `tools/check_save_clobber.py` (overwrites its own save window).

### Why recovery is slow, and the accelerator

`recover_gaps.py` recovers one function per gap per pass, because `end` is only
an upper bound and codegen stops at the first function in the range. Where a gap
holds a *run* of identically-sized functions - this image has long tables of
0x18-byte wrappers - that costs one full codegen, about three minutes, per
function.

`tools/fill_chains.py` handles those directly: it takes the stride from the
function immediately before a gap, checks the gap is a whole number of strides,
and declares every slot at once. It deliberately skips gaps whose length is not
a multiple of the stride and leaves them to the normal loop. One run of nine
wrappers collapses nine passes into one.

The two are complementary and are alternated, with `prune_epilogue_gaps.py`
between them so nothing malformed survives into a build.

### The fix

`tools/prune_epilogue_gaps.py --apply` removes any declaration whose generated
body fails three tests a real function never fails: it restores callee-saved
registers it never saved, it has no terminator, or it writes through r31 without
setting it. It pruned 37 of 549. `tools/recover_gaps.py` now applies the same
tests every pass, so the loop cannot reintroduce them.

**A correction worth recording:** earlier notes in this file claimed the gap
declarations were ruled out, because every function in the crash call stack was
analyzer-discovered. That was true and beside the point. The damage came from a
fragment reached by *indirect dispatch*, which never appears in the stack that
finally crashes. Checking the call stack was not a sufficient test, and stating
the exoneration that confidently was wrong.

### Result

The `0x68EC0000` failure is gone. Startup now stops later, on a missing function
at `0x82B09CB0` - one of the addresses the prune removed, which is genuinely
called and needs a correctly-bounded declaration rather than a fragment. The
recovery loop, with the fragment filter now active, is the mechanism for that.

## Tooling

Built during the startup-bug hunt; all are read-only except where noted.

- `tools/check_nonvolatile.py` — whole-image scan: writes a callee-saved register without saving it
- `tools/check_save_clobber.py` — whole-image scan: overwrites its own register-save window
- `tools/fill_chains.py` — declares a whole run of same-sized functions in one pass
- `tools/probe_r1.py` — generates stack-pointer probes for named functions
- `tools/probe_sites.py` — generates a uniquely-named probe per call site, for attribution
- `tools/prune_epilogue_gaps.py` — removes declarations that are fragments rather than functions
- `tools/recover_gaps.py` — the per-gap recovery loop, with the fragment tests built in

## Verified environment facts

Checked on this machine rather than assumed:

- **Vulkan works.** `vulkaninfo --summary` enumerates the AMD Radeon 780M at
  Vulkan 1.4.344 on the AMD proprietary driver. Note that the ICD is *not*
  registered under `HKLM\SOFTWARE\Khronos\Vulkan\Drivers` — that key does not
  exist here — so checking the registry gives a false negative. Run
  `vulkaninfo` instead.
- **`rexglue --log-file` appends**, it does not truncate. A log that looks like
  it contains errors may be holding output from an earlier run. Delete the file
  or compare timestamps before believing it.
- **`rexglue init achievements` still requires the parent command's flags**, so
  the invocation is
  `rexglue init --project-name X --xex-path <xex> achievements <xex> <outdir>`.

## Notes for the Xbox port

The eventual target is original Xbox, which changes the constraints
substantially — 64 MB unified memory against the 360's 512 MB, a 733 MHz
Pentium III against three 3.2 GHz PowerPC cores, and fixed-function-era shaders
against the Xenos. Nothing about the current build is Xbox-ready; it exists to
establish a correct reference to diff against.

Two things done here are worth keeping for that work:

- Guest paths in the manifest use the real `Root` casing rather than the
  lowercased form `rexglue init` emitted. Windows does not care; a
  case-sensitive host will.
- `nbajam_ofe_config.toml` is a layered include rather than inline manifest
  edits, so per-target override sets can be stacked later.
