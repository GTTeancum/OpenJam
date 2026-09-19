#!/usr/bin/env python3
"""Read and edit the front end: EA's APT screens, and the main menu in particular.

Every screen in this game is an `.ast` archive holding a pair of blobs: an
"Apt constant file" (a string pool) and an "Apt Data" blob (a Flash-like movie
with ActionScript bytecode). The bytecode is SWF's, renumbered, with EA's own
additions. The opcode numbering and the 4-byte alignment rule here come from
feliwir's libapt, which documents the format SAGE-era EA titles used; this
build's screens are the same format at version 1:7:4.

**Why this file exists.** Every internal reference in an Apt Data blob is an
absolute position, so inserting or deleting a single byte invalidates the whole
file. That makes arbitrary editing a large job. What this does instead is edit
*within* a function's existing code budget: the main menu is built by one
function that assembles an array of item objects, each item a self-contained
run of bytecode ending in `array.push(item)`. Those runs can be reordered and
dropped freely, and the leftover space is padded out after the function's
`return`, where nothing ever executes. The function's length never changes, so
no offset anywhere in the file has to move.

**Checking the disassembler.** Each function declares its own code length, so a
wrong operand size derails the stream and lands off the end. All 39 functions
in mainmenu.ast decode to exactly their stated length, which is what makes the
opcode table below trustworthy rather than merely plausible.

Usage:
    python tools/apt.py list   <screen.ast>
    python tools/apt.py items  <screen.ast>
    python tools/apt.py mainmenu <in.ast> <out.ast>
"""

import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ast_repack

MAGIC = bytes.fromhex("9876543212345678")   # sits between a function header and its code

OPCODES = {
    0x00: "END", 0x04: "NEXTFRAME", 0x05: "PREVFRAME", 0x06: "PLAY", 0x07: "STOP",
    0x08: "TOGGLEQUALITY", 0x09: "STOPSOUNDS", 0x0A: "ADD", 0x0B: "SUBTRACT",
    0x0C: "MULTIPLY", 0x0D: "DIVIDE", 0x0E: "EQUALS", 0x0F: "LESSTHAN", 0x10: "AND",
    0x11: "OR", 0x12: "NOT", 0x13: "STRINGEQUALS", 0x14: "STRINGLENGTH",
    0x15: "SUBSTRING", 0x17: "POP", 0x18: "TO_INT", 0x1C: "GETVARIABLE",
    0x1D: "SETVARIABLE", 0x21: "STRINGCONCAT", 0x22: "GETPROPERTY",
    0x23: "SETPROPERTY", 0x24: "CLONE_SPRITE", 0x25: "REMOVE_SPRITE", 0x26: "TRACE",
    0x27: "STARTDRAG", 0x28: "STOPDRAG", 0x29: "STRINGCOMPARE", 0x2A: "THROW",
    0x2B: "CASTOP", 0x2C: "IMPLEMENTSOP", 0x30: "RANDOM", 0x31: "MBLENGTH",
    0x32: "ORD", 0x33: "CHR", 0x34: "GETTIMER", 0x35: "MBSUBSTRING", 0x36: "MBORD",
    0x37: "MBCHR", 0x3A: "DELETE", 0x3C: "DEFINELOCAL", 0x3D: "CALLFUNCTION",
    0x3E: "RETURN", 0x3F: "MODULO", 0x40: "NEW", 0x41: "VAR", 0x42: "INITARRAY",
    0x43: "INITOBJECT", 0x44: "TYPEOF", 0x45: "TARGETPATH", 0x46: "ENUMERATE",
    0x47: "ADD2", 0x48: "LESSTHAN2", 0x49: "EQUALS2", 0x4A: "TONUMBER",
    0x4B: "TOSTRING", 0x4C: "PUSHDUPLICATE", 0x4D: "STACKSWAP", 0x4E: "GETMEMBER",
    0x4F: "SETMEMBER", 0x50: "INCREMENT", 0x51: "DECREMENT", 0x52: "CALLMETHOD",
    0x53: "NEWMETHOD", 0x54: "INSTANCEOF", 0x55: "ENUMERATE2", 0x56: "PUSHTHIS",
    0x58: "PUSHGLOBAL", 0x59: "PUSHZERO", 0x5A: "PUSHONE", 0x5B: "CALLFUNCNPOP",
    0x5C: "CALLFUNC", 0x5D: "CALLMETHODPOP", 0x5E: "CALLMETHOD2", 0x60: "BITAND",
    0x61: "BITOR", 0x62: "BITXOR", 0x63: "SHL", 0x64: "SHR", 0x65: "USHR",
    0x66: "STRICTEQ", 0x67: "GREATER", 0x68: "STRINGGREATER", 0x69: "EXTENDS",
    0x70: "PUSHTHISVAR", 0x71: "PUSHGLOBALVAR", 0x72: "ZEROVAR", 0x73: "PUSHTRUE",
    0x74: "PUSHFALSE", 0x75: "PUSHNULL", 0x76: "PUSHUNDEFINED", 0x77: "TRACE_START",
    0x81: "GOTOFRAME", 0x83: "GETURL", 0x87: "SETREGISTER", 0x88: "CONSTANTPOOL",
    0x8A: "WAITFORFRAME", 0x8B: "SETTARGET", 0x8C: "GOTOLABEL",
    0x8D: "WAITFORFRAMEEXPR", 0x8E: "DEFINEFUNCTION2", 0x8F: "TRY", 0x94: "WITH",
    0x96: "PUSHDATA", 0x99: "BRANCHALWAYS", 0x9A: "GETURL2", 0x9B: "DEFINEFUNCTION",
    0x9D: "BRANCHIFTRUE", 0x9E: "CALLFRAME", 0x9F: "GOTOFRAME2", 0xA1: "PUSHSTRING",
    0xA2: "PUSHCONST8", 0xA3: "PUSHCONST16", 0xA4: "GETSTRINGVAR",
    0xA5: "GETSTRINGMEMBER", 0xA6: "SETSTRINGVAR", 0xA7: "SETSTRINGMEMBER",
    0xAE: "PUSHVALUEOFVAR", 0xAF: "GETNAMEDMEMBER", 0xB0: "CALLNAMEDFUNCPOP",
    0xB1: "CALLNAMEDFUNC", 0xB2: "CALLNAMEDMETHODPOP", 0xB3: "CALLNAMEDMETHOD",
    0xB4: "PUSHFLOAT", 0xB5: "PUSHBYTE", 0xB6: "PUSHSHORT", 0xB7: "PUSHLONG",
    0xB8: "BRANCHIFFALSE", 0xB9: "PUSHREGISTER",
}

# Operands of these are read at the next 4-byte boundary.
ALIGNED = {0x81, 0x83, 0x87, 0x88, 0x8C, 0x8E, 0x96, 0x99, 0x9B, 0x9D,
           0xA1, 0xA4, 0xA5, 0xA6, 0xA7, 0xB8}
# CONSTANTPOOL is the one instruction with two words after it: how many
# constants, and where the list of their indices lives.
U32X2 = {0x88}
U32 = {0x81, 0x83, 0x87, 0x8C, 0x96, 0xA1, 0xA4, 0xA5, 0xA6, 0xA7, 0xB4, 0xB7}
S32 = {0x99, 0x9D, 0xB8}
U16 = {0xA3, 0xB6}
U8 = {0xA2, 0xAE, 0xAF, 0xB0, 0xB1, 0xB2, 0xB3, 0xB5, 0xB9}
# Operands of these name a constant rather than a number.
NAMES_A_CONSTANT = {0xA2, 0xA3, 0xAE, 0xAF, 0xB0, 0xB1, 0xB2, 0xB3}


class Screen:
    """One front-end screen: its string pool and its movie."""

    def __init__(self, path):
        self.path = path
        self.archive = ast_repack.Archive(path)
        self.apt = self.apt_index = None
        self.const = None
        for e in self.archive.entries:
            b = self.archive.read(e)
            if b[:8] == b"Apt Data":
                self.apt, self.apt_index = bytearray(b), e["index"]
            elif b[:17] == b"Apt constant file":
                self.const = b
        if self.apt is None or self.const is None:
            raise ValueError("{}: not an APT screen".format(path))
        self.constants = self._constants()

    def _constants(self):
        n = struct.unpack_from(">I", self.const, 0x18)[0]
        out = []
        for i in range(n):
            typ, off = struct.unpack_from(">II", self.const, 0x20 + 8 * i)
            out.append(self._string(self.const, off) if typ == 1 else "<%d>" % typ)
        return out

    @staticmethod
    def _string(buf, off):
        if not off or off >= len(buf):
            return ""
        end = buf.index(b"\0", off)
        return buf[off:end].decode("latin-1")

    def constant(self, name):
        return self.constants.index(name)

    def functions(self):
        """Every function body, found by the sentinel that precedes its code."""
        out, pos = [], 0
        while True:
            m = self.apt.find(MAGIC, pos)
            if m < 0:
                return out
            pos = m + 8
            head = m - 20
            if head < 1:
                continue
            name_off, nparams, _flags, _params, size = struct.unpack_from(
                ">5I", self.apt, head)
            op = head - 1
            while op > 0 and self.apt[op] == 0:      # back over the alignment padding
                op -= 1
            if self.apt[op] not in (0x8E, 0x9B):
                continue
            out.append({"at": op, "name": self._string(self.apt, name_off),
                        "nparams": nparams, "code": m + 8, "size": size})

    def disassemble(self, start, size):
        """[(offset, opcode, operand)], with nested function bodies stepped over."""
        out, p, end = [], start, start + size
        while p < end:
            op, at = self.apt[p], p
            p += 1
            if op in ALIGNED:
                p = (p + 3) & ~3
            if op in (0x8E, 0x9B):
                _n, _np, _f, _pp, csize = struct.unpack_from(">5I", self.apt, p)
                p += 20 + 8 + csize
                out.append((at, op, csize))
                continue
            arg = None
            if op in U32X2:
                arg = struct.unpack_from(">I", self.apt, p)[0]; p += 8
            elif op in U32:
                arg = struct.unpack_from(">I", self.apt, p)[0]; p += 4
            elif op in S32:
                arg = struct.unpack_from(">i", self.apt, p)[0]; p += 4
            elif op in U16:
                arg = struct.unpack_from(">H", self.apt, p)[0]; p += 2
            elif op in U8:
                arg = self.apt[p]; p += 1
            if op not in OPCODES:
                raise ValueError("unknown opcode 0x%02X at 0x%X" % (op, at))
            out.append((at, op, arg))
        if p != end:
            raise ValueError("code at 0x%X overran its %d bytes" % (start, size))
        return out

    def text(self, ins):
        at, op, arg = ins
        s = OPCODES[op]
        if arg is None:
            return s
        if op in NAMES_A_CONSTANT and arg < len(self.constants):
            return "%s %d ; %s" % (s, arg, self.constants[arg])
        return "%s %d" % (s, arg)

    def save(self, path):
        self.archive.write(path, {self.apt_index: bytes(self.apt)})


def menu_function(scr):
    """The one that builds the Xbox Arcade menu: the biggest in the screen."""
    return max(scr.functions(), key=lambda f: f["size"])


def menu_items(scr, fn):
    """Split the builder into one byte range per `array.push(item)`."""
    ins = scr.disassemble(fn["code"], fn["size"])
    push = scr.constant("push")
    items, start = [], fn["code"]
    for i, (at, op, arg) in enumerate(ins):
        if op == 0xB2 and arg == push:              # CALLNAMEDMETHODPOP "push"
            end = ins[i + 1][0] if i + 1 < len(ins) else fn["code"] + fn["size"]
            body = [x for x in ins if start <= x[0] < end]
            tags = [scr.constants[a] for _, o, a in body
                    if o in NAMES_A_CONSTANT and a < len(scr.constants)]
            loc = [t for t in tags if t.startswith("HAL_")
                   and not t.endswith("_DESCRIPTION")]
            reg = [a for _, o, a in body if o == 0xB9]
            items.append({"start": start, "end": end, "ins": body,
                          "label": loc[0] if loc else "?",
                          "icon": "IconAssetID" in tags,
                          "array": reg[-1] if reg else None})
            start = end
    tail = (start, fn["code"] + fn["size"])
    return items, tail


def strip_icon(scr, item):
    """Return the item's bytes with its icon field removed.

    An icon entry differs from a text entry by exactly two instructions at the
    front - `PUSHCONST8 IconAssetID` and the value that follows it - plus the
    field count that `INITOBJECT` consumes, which has to come down by one.
    """
    body = scr.apt[item["start"]:item["end"]]
    ins = item["ins"]
    icon = scr.constant("IconAssetID")
    if not (ins[0][1] == 0xA2 and ins[0][2] == icon):
        raise ValueError("%s does not start with an icon field" % item["label"])
    cut = ins[2][0] - ins[0][0]                      # the field name and its value
    out = bytearray(body[cut:])
    # The last PUSHBYTE before INITOBJECT is the number of fields in the object.
    for i in range(len(ins) - 1, 0, -1):
        if ins[i][1] == 0x43 and ins[i - 1][1] == 0xB5:
            pos = ins[i - 1][0] - item["start"] - cut
            out[pos + 1] -= 1
            return bytes(out)
    raise ValueError("%s has no field count to adjust" % item["label"])


def relabel(scr, body, old, new):
    """Point an entry at a different HAL_ constant, and its description with it."""
    out = bytearray(body)
    for a, b in ((old, new), (old + "_DESCRIPTION", new + "_DESCRIPTION")):
        oi, ni = scr.constant(a), scr.constant(b)
        for p in range(len(out) - 1):
            if out[p] == 0xAF and out[p + 1] == oi:  # GETNAMEDMEMBER
                out[p + 1] = ni
                break
        else:
            raise ValueError("could not find %s to relabel" % a)
    return bytes(out)


# The three Xbox Live entries in the icon row. All are dead: the services
# behind party sessions, leaderboards and achievements are gone.
DEAD_XBOX_LIVE = ("HAL_XBOX_LIVE_PARTY_SESSIONS", "HAL_LEADERBOARDS",
                  "HAL_ACHIEVEMENTS")


def neutral_filler(n):
    """`n` bytes of instructions that leave the stack exactly as they found it.

    Pushing and popping in equal measure costs one frame's worth of nothing
    when the menu is built, and lets an entry be removed without the run of
    bytecode around it changing length.
    """
    out = bytearray()
    if n % 2:
        out += bytes([0xB5, 0x00, 0x17])        # PUSHBYTE 0, POP
        n -= 3
    out += bytes([0x59, 0x17]) * (n // 2)       # PUSHZERO, POP
    return bytes(out)


def cmd_mainmenu(args):
    """Drop entries from the main menu without changing any length.

    Each entry is a self-contained run of bytecode that builds an object and
    appends it to the menu array, so overwriting one with instructions that do
    nothing removes it from the menu and leaves every byte position in the file
    exactly where it was.

    That matters more than it sounds. Removing the bytes outright and
    recomputing every position in the payload - which tools/apt_movie.py can do,
    and does correctly by every check available - still leaves the screen blank
    or faulting on a raw file offset. Something in the loader rebuilds pointers
    from the file in a way this does not yet account for. Until that is
    understood, an edit that moves nothing is the one that can be trusted, and
    it is enough for this job.
    """
    scr = Screen(args.source)
    fn = menu_function(scr)
    items, _ = menu_items(scr, fn)
    drop = set(args.drop or DEAD_XBOX_LIVE)

    known = {i["label"] for i in items}
    missing = drop - known
    if missing:
        raise SystemExit("this screen has no %s" % ", ".join(sorted(missing)))

    print("%s: menu builder at 0x%05X, %d bytes" %
          (os.path.basename(args.source), fn["at"], fn["size"]))
    for it in items:
        if it["label"] in drop:
            n = it["end"] - it["start"]
            scr.apt[it["start"]:it["end"]] = neutral_filler(n)
            print("   removed %-30s (%d bytes, replaced in place)" % (it["label"], n))

    scr.disassemble(fn["code"], fn["size"])     # must still decode cleanly
    after, _ = menu_items(scr, fn)
    print("   menu is now: %s" %
          ", ".join(i["label"].replace("HAL_", "") for i in after if i["array"] == 2))
    scr.save(args.dest)
    print("   wrote %s" % args.dest)
    return 0


def cmd_list(args):
    scr = Screen(args.source)
    print("%d constants, %d functions" % (len(scr.constants), len(scr.functions())))
    for f in scr.functions():
        print("  0x%05X  %-26s params %d  code 0x%05X  %5d bytes"
              % (f["at"], f["name"] or "(anonymous)", f["nparams"], f["code"], f["size"]))
    return 0


def cmd_items(args):
    scr = Screen(args.source)
    fn = menu_function(scr)
    items, _ = menu_items(scr, fn)
    for i, it in enumerate(items):
        print("  %2d  array %s  %-32s %-5s  0x%05X..0x%05X"
              % (i + 1, it["array"], it["label"], "icon" if it["icon"] else "text",
                 it["start"], it["end"]))
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("list"); p.add_argument("source"); p.set_defaults(func=cmd_list)
    p = sub.add_parser("items"); p.add_argument("source"); p.set_defaults(func=cmd_items)
    p = sub.add_parser("mainmenu", help="drop dead entries from the main menu")
    p.add_argument("source"); p.add_argument("dest")
    p.add_argument("--drop", action="append", metavar="HAL_NAME",
                   help="entry to remove (repeatable; defaults to the dead "
                        "Xbox Live trio)")
    p.set_defaults(func=cmd_mainmenu)
    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
