#!/usr/bin/env python3
"""Remove declared gap functions that are really epilogue fragments.

tools/recover_gaps.py declares a function at the start of every uncovered run of
bytes. Most of those are genuine functions discovery missed. Some are not: they
are the *tail* of a function whose body the analyzer already covered, so the
declared "function" consists only of an epilogue - restoring r30/r31 and
branching to the link register, with no matching prologue.

Declaring one of those is actively harmful. The address gets registered in the
guest function table, and when anything dispatches to it the restore reads stack
slots that were never written for the live frame. On this image those slots hold
0xBE, the pattern the runtime fills new thread stacks with, so a caller's
non-volatile register comes back as 0xBEBEBEBE. That is what destroys the guest
stack pointer during the CRT static-initializer pass: sub_82558718 keeps its
frame pointer in r31 and restores the stack with `addi r1,r31,176`.

A declared function is pruned when it *reads* a non-volatile register back from
the stack (or calls __restgprlr) without ever saving it. Real compiler output
never does that; only a fragment does.

Usage:
    python tools/prune_epilogue_gaps.py           # report only
    python tools/prune_epilogue_gaps.py --apply   # rewrite the TOML and ban list
"""

import argparse
import io
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GEN = ROOT / "generated" / "default"
GAPS = ROOT / "nbajam_ofe_gaps.toml"
BANLIST = ROOT / "tools" / "gap_banlist.txt"

FN = re.compile(r"^DEFINE_REX_FUNC\(([A-Za-z_][A-Za-z0-9_]*)\)")
DECL = re.compile(r'^"0x([0-9A-F]+)"\s*=\s*\{\s*end\s*=\s*0x([0-9A-F]+)')
SAVE = re.compile(r"__savegprlr_(\d+)\(ctx, base\)")
REST = re.compile(r"__restgprlr_(\d+)\(ctx, base\)")

NONVOL = range(14, 32)


def collect_bodies():
    out = {}
    for path in sorted(GEN.glob("nbajam_ofe_recomp.*.cpp")):
        lines = io.open(path, encoding="utf-8", errors="replace").read().split("\n")
        starts = [i for i, l in enumerate(lines) if FN.match(l)]
        for idx, s in enumerate(starts):
            e = starts[idx + 1] if idx + 1 < len(starts) else len(lines)
            name = FN.match(lines[s]).group(1)
            out[name] = "\n".join(lines[s:e])
    return out


def is_fragment(text):
    """True if this is a piece of a function rather than a whole one.

    Three tests, each of which a real function never fails:

    1. It restores callee-saved registers it never saved. That is an epilogue
       without its prologue.
    2. It has no terminator - the body just runs off the end with no `blr` and
       no tail call. A real function always ends in one or the other.
    3. It writes through r31 without ever setting r31. r31 is the frame pointer
       in this image's calling convention, so such a fragment scribbles into
       whatever frame happens to be live. Two of these landed inside the
       register-save window of a 176-byte frame, which is how the guest stack
       pointer was being destroyed.
    """
    if REST.search(text) and not SAVE.search(text):
        return True, "calls __restgprlr with no __savegprlr"

    for n in NONVOL:
        loads = re.search(r"ctx\.r{}\.u64 = REX_LOAD_U64".format(n), text)
        if not loads:
            continue
        saved = re.search(r"REX_STORE_U64\([^,]+,\s*ctx\.r{}\.u64\)".format(n), text)
        helper = any(int(m) <= n for m in SAVE.findall(text))
        if not saved and not helper:
            return True, "restores r{} from the stack without saving it".format(n)

    lines = [l.strip() for l in text.split("\n") if l.strip() and l.strip() != "}"]
    if lines and lines[-1] != "return;":
        return True, "no terminator; body runs off the end"

    sets_fp = re.search(r"ctx\.r31\.(?:u32|u64|s64)\s*=", text)
    writes_fp = re.search(r"REX_STORE_U\d+\(ctx\.r31\.u32", text)
    if writes_fp and not sets_fp:
        return True, "writes through r31 without ever setting it"

    return False, ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--apply", action="store_true")
    args = ap.parse_args()

    bodies = collect_bodies()
    decls = {}
    for line in io.open(GAPS, encoding="utf-8").read().splitlines():
        m = DECL.match(line)
        if m:
            decls[int(m.group(1), 16)] = int(m.group(2), 16)

    bad = []
    for addr in sorted(decls):
        text = bodies.get("sub_{:08X}".format(addr))
        if text is None:
            continue
        frag, why = is_fragment(text)
        if frag:
            bad.append((addr, why))

    print("{} declared gap functions, {} are epilogue fragments\n".format(len(decls), len(bad)))
    for addr, why in bad:
        print("  0x{:08X}  {}".format(addr, why))

    if not bad:
        return 0
    if not args.apply:
        print("\n(report only; pass --apply to prune)")
        return 0

    drop = {a for a, _ in bad}
    kept = {a: e for a, e in decls.items() if a not in drop}

    header = [
        "# NBA JAM: On Fire Edition - recovered gap functions",
        "#",
        "# Generated by tools/recover_gaps.py, then pruned by",
        "# tools/prune_epilogue_gaps.py. `end` is an upper bound; codegen stops at",
        "# the real end of each function. See BUILDING.md.",
        "#",
        "# {} entries.".format(len(kept)),
        "",
        "[functions]",
    ]
    body = ['"0x{:08X}" = {{ end = 0x{:08X} }}'.format(a, e) for a, e in sorted(kept.items())]
    io.open(GAPS, "w", encoding="utf-8", newline="\n").write("\n".join(header + body) + "\n")

    banned = set()
    if BANLIST.exists():
        for line in io.open(BANLIST, encoding="utf-8").read().splitlines():
            tok = line.split("#")[0].strip()
            if tok:
                banned.add(int(tok, 16))
    banned |= drop
    io.open(BANLIST, "w", encoding="utf-8", newline="\n").write(
        "# Gap addresses that must not be declared. Either codegen could not\n"
        "# decode them, or the declared bounds cut a real function, or the range\n"
        "# is only a function epilogue -- declaring one of those registers an\n"
        "# address that restores non-volatile registers it never saved, which\n"
        "# corrupts the caller's frame pointer.\n"
        + "\n".join("{:08X}".format(a) for a in sorted(banned)) + "\n")

    print("\npruned {} entries; {} remain; ban list now {} addresses".format(
        len(drop), len(kept), len(banned)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
