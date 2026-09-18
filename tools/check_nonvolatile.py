#!/usr/bin/env python3
"""Find recompiled functions that clobber a non-volatile register.

On PPC, r14-r31 are callee-saved: a function may use them only if it saves and
restores them. Real compiler output always does. If a *recompiled* function
writes one without saving it, the recompilation is wrong - usually because the
function's real entry point is earlier than the analyzer thought, so the
prologue that did the saving was never included.

That is the shape of the bug being chased. `sub_82558718` sets r31 to its frame
pointer, calls out, and restores its stack pointer with `addi r1,r31,176`. It
gets 0xBEBEBEBE, the pattern the runtime fills new stacks with, so something it
called overwrote r31 and never put it back.

A function is reported when it writes rN (14 <= N <= 31) and there is no
evidence it preserved it: no `__savegprlr_M` with M <= N, and no 64-bit store of
that register.

Usage:  python tools/check_nonvolatile.py [--reg 31] [--limit 40]
"""

import argparse
import glob
import io
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GEN = ROOT / "generated" / "default"

FN = re.compile(r"^DEFINE_REX_FUNC\(([A-Za-z_][A-Za-z0-9_]*)\)")
SAVE = re.compile(r"__savegprlr_(\d+)\(ctx, base\)")
REST = re.compile(r"__restgprlr_(\d+)\(ctx, base\)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--reg", type=int, default=31)
    ap.add_argument("--limit", type=int, default=40)
    args = ap.parse_args()

    n = args.reg
    write_rx = re.compile(r"ctx\.r{}\.(?:u32|u64|s64)\s*=".format(n))
    store_rx = re.compile(r"REX_STORE_U64\([^,]+,\s*ctx\.r{}\.u64\)".format(n))
    load_rx = re.compile(r"ctx\.r{}\.u64 = REX_LOAD_U64".format(n))

    flagged = []
    total = 0
    for path in sorted(GEN.glob("nbajam_ofe_recomp.*.cpp")):
        lines = io.open(path, encoding="utf-8", errors="replace").read().split("\n")
        starts = [i for i, l in enumerate(lines) if FN.match(l)]
        for idx, s in enumerate(starts):
            e = starts[idx + 1] if idx + 1 < len(starts) else len(lines)
            body = lines[s:e]
            name = FN.match(body[0]).group(1)
            if name.startswith("__savegprlr") or name.startswith("__restgprlr"):
                continue
            text = "\n".join(body)
            total += 1

            if not write_rx.search(text):
                continue

            # Preserved via the helper chain?
            saved = any(int(m) <= n for m in SAVE.findall(text))
            restored = any(int(m) <= n for m in REST.findall(text))
            # Or explicitly stored/loaded.
            stored = bool(store_rx.search(text))
            loaded = bool(load_rx.search(text))

            if saved or stored:
                continue
            flagged.append((name, path.name, len(body), saved, restored, stored, loaded))

    print("scanned {} functions for r{} clobbering\n".format(total, n))
    if not flagged:
        print("no function writes r{} without saving it".format(n))
        return 0

    print("{} function(s) write r{} with no save:\n".format(len(flagged), n))
    for name, fname, ln, saved, restored, stored, loaded in flagged[: args.limit]:
        note = []
        if restored:
            note.append("calls __restgprlr (restores without saving!)")
        if loaded:
            note.append("loads r{} from stack".format(n))
        print("  {:22} {:28} {:4} lines   {}".format(
            name, fname, ln, "; ".join(note) if note else ""))
    if len(flagged) > args.limit:
        print("  ... and {} more".format(len(flagged) - args.limit))
    return 0


if __name__ == "__main__":
    sys.exit(main())
