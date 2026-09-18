#!/usr/bin/env python3
"""Repair vector instructions ReXGlue translates unsafely when the destination
register is also a source. Runs after codegen, before the sources are compiled.

ReXGlue v0.10.0 builds `vpkuwus` and `vpkuhus` as eight separate assignments
into the destination register, interleaved with reads of the two source
registers:

    vD.u16[7-i] = saturate(vA.u32[3-i]);
    vD.u16[3-i] = saturate(vB.u32[3-i]);

That is correct only when vD is neither vA nor vB. When it is, those writes
land on bytes the later reads still need, and half the result is garbage.

This is what breaks the EA Sports intro. Its VP6 motion compensation filters
finish with a pack, and all three `vpkuwus128` in the image have the
destination as one of their sources. Replacing just those three takes the
decoded frame from 23% of pixels wrong to 0.08% against an ffmpeg decode of the
same file. The game uses the instruction nowhere else, which is why only the
video was affected.

Each unsafe instruction is rewritten to call src/rexglue_vector_fixups.h, which
computes the whole result before storing any of it. Instructions whose
destination is a distinct register are left exactly as codegen emitted them.

CMake runs this automatically. It is idempotent and does not touch a file whose
content would not change, so it never forces a rebuild on its own.

Usage:
    python tools/fix_vector_aliasing.py            # report
    python tools/fix_vector_aliasing.py --apply
"""

import argparse
import io
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GEN = ROOT / "generated" / "default"
INCLUDE = '#include "../../src/rexglue_vector_fixups.h"'

INSN = re.compile(r"^\t// ([a-z][a-z0-9_.]*)\s*(.*)$")
VREG = re.compile(r"^v(\d+)$")
LABEL = re.compile(r"^\s*loc_[0-9A-Fa-f]+:\s*$")

# opcode, with and without the VMX128 suffix -> replacement function
UNSAFE = {
    "vpkuwus": "rexfix::vpkuwus",
    "vpkuwus128": "rexfix::vpkuwus",
    "vpkuhus": "rexfix::vpkuhus",
    "vpkuhus128": "rexfix::vpkuhus",
}


def operands(text):
    return [t.strip() for t in text.split("//")[0].strip().split(",") if t.strip()]


def fix_file(path, apply):
    """Returns (rewritten_now, already_rewritten)."""
    text = io.open(path, encoding="utf-8", errors="replace").read()
    if not any(("// " + op) in text for op in UNSAFE):
        return [], []

    lines = text.split("\n")
    out, found, already = [], [], []
    i = 0
    while i < len(lines):
        m = INSN.match(lines[i])
        if not m or m.group(1) not in UNSAFE:
            out.append(lines[i])
            i += 1
            continue

        op = m.group(1)
        regs = [t for t in operands(m.group(2)) if VREG.match(t)]
        # The emitted body runs to the next instruction comment. Any branch
        # label inside it has to survive, or the goto targeting it stops
        # compiling.
        j = i + 1
        labels = []
        while j < len(lines) and not INSN.match(lines[j]):
            if LABEL.match(lines[j]):
                labels.append(lines[j])
            j += 1

        done = any("rexfix::" in lines[k] for k in range(i + 1, j))
        aliases = len(regs) >= 3 and regs[0] in regs[1:]
        if done:
            already.append((op, m.group(2).strip()))
            out.extend(lines[i:j])
        elif not aliases:
            out.extend(lines[i:j])
        else:
            found.append((op, m.group(2).strip()))
            out.append(lines[i])
            out.append("\t{}(ctx.{}, ctx.{}, ctx.{});".format(
                UNSAFE[op], regs[0], regs[1], regs[2]))
            out.extend(labels)
        i = j

    if found and apply:
        body = "\n".join(out)
        if INCLUDE not in body:
            first_func = body.find("DEFINE_REX_FUNC")
            idx = body.rfind("#include", 0, first_func if first_func > 0 else len(body))
            eol = body.find("\n", idx)
            body = body[:eol + 1] + INCLUDE + "\n" + body[eol + 1:]
        # Only write when the content actually differs. Rewriting identical
        # bytes would move the timestamp and rebuild the world on every build.
        if body != text:
            io.open(path, "w", encoding="utf-8", newline="\n").write(body)
    return found, already


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--apply", action="store_true")
    args = ap.parse_args(argv)

    if not GEN.is_dir():
        print("no generated sources at {}".format(GEN))
        return 1

    total = 0
    done = 0
    for path in sorted(GEN.glob("nbajam_ofe_recomp.*.cpp")):
        found, already = fix_file(path, args.apply)
        for op, text in found:
            print("  {}  {} {}".format(path.name, op, text))
        total += len(found)
        done += len(already)

    if total == 0 and done == 0:
        print("no unsafe pack instructions in the generated sources")
    elif total == 0:
        print("{} instruction(s) already repaired; nothing to do".format(done))
    else:
        print("{} instruction(s) {}".format(total, "rewritten" if args.apply else "to rewrite"))
        if not args.apply:
            print("(report only; pass --apply)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
