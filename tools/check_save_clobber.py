#!/usr/bin/env python3
"""Find recompiled functions that overwrite their own saved registers.

The PPC helpers `__savegprlr_N` store rN..r31 to [r1-8*(32-N)-8 .. r1-8) using
the stack pointer *before* the frame is pushed. After `stwu r1,-F(r1)` that save
area sits inside the function's own frame, at offsets [F-8*(32-N)-8, F-8) from
the new r1. Anything the body stores into that window destroys the saved
registers, and the matching `__restgprlr_N` then restores garbage.

That is exactly the failure being chased: `sub_82558718` restores its stack
pointer from r31 (`addi r1,r31,176`) and gets 0xBEBEBEBE, the fill pattern the
runtime paints new stacks with.

This scans every recompiled function, works out its save window, and reports any
store that lands inside it. Stores are counted through both r1 and r31, since
frame-pointer functions address locals through r31.

Usage:  python tools/check_save_clobber.py
"""

import glob
import io
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GEN = ROOT / "generated" / "default"

FN = re.compile(r"^DEFINE_REX_FUNC\(([A-Za-z_][A-Za-z0-9_]*)\)")
SAVE = re.compile(r"__savegprlr_(\d+)\(ctx, base\)")
# stwu r1,-F(r1)
STWU = re.compile(r"^\tea = -(\d+) \+ ctx\.r1\.u32;")
# addi r31,r1,-F   (frame pointer set up before the push)
FP = re.compile(r"^\tctx\.r31\.s64 = ctx\.r1\.s64 \+ -(\d+);")
STORE_R1 = re.compile(r"REX_STORE_U(?:8|16|32|64)\(ctx\.r1\.u32 \+ (-?\d+)")
STORE_R31 = re.compile(r"REX_STORE_U(?:8|16|32|64)\(ctx\.r31\.u32 \+ (-?\d+)")


def main():
    findings = []
    scanned = 0
    for path in sorted(GEN.glob("nbajam_ofe_recomp.*.cpp")):
        lines = io.open(path, encoding="utf-8", errors="replace").read().split("\n")
        starts = [i for i, l in enumerate(lines) if FN.match(l)]
        for idx, s in enumerate(starts):
            e = starts[idx + 1] if idx + 1 < len(starts) else len(lines)
            body = lines[s:e]
            name = FN.match(body[0]).group(1)
            text = "\n".join(body)

            m = SAVE.search(text)
            if not m:
                continue
            n = int(m.group(1))
            scanned += 1
            save_bytes = 8 * (32 - n) + 8          # rN..r31 plus the lr slot

            mf = STWU.search(text) or None
            for l in body:
                mm = STWU.match(l)
                if mm:
                    mf = mm
                    break
            if not mf:
                continue
            frame = int(mf.group(1))

            lo = frame - save_bytes                # inclusive, from new r1
            hi = frame - 8                         # exclusive
            if lo < 0:
                continue

            has_fp = any(FP.match(l) for l in body)

            hits = []
            for l in body:
                for rx, via in ((STORE_R1, "r1"), (STORE_R31, "r31")):
                    for off in rx.findall(l):
                        off = int(off)
                        if via == "r31" and not has_fp:
                            continue
                        if lo <= off < hi:
                            hits.append((via, off, l.strip()))
            if hits:
                findings.append((name, path.name, n, frame, lo, hi, hits))

    print("scanned {} functions that use __savegprlr\n".format(scanned))
    if not findings:
        print("no function stores into its own register-save window")
        return 0

    for name, fname, n, frame, lo, hi, hits in findings:
        print("*** {}  ({})".format(name, fname))
        print("    __savegprlr_{}, frame {}, save window [{}, {}) from r1".format(
            n, frame, lo, hi))
        for via, off, src in hits[:6]:
            print("      store via {} at +{}: {}".format(via, off, src))
        print()
    print("{} function(s) flagged".format(len(findings)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
