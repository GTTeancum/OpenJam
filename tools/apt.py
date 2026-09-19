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
import locdb

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
        self.const = self.const_index = None
        for e in self.archive.entries:
            b = self.archive.read(e)
            if b[:8] == b"Apt Data":
                self.apt, self.apt_index = bytearray(b), e["index"]
            elif b[:17] == b"Apt constant file":
                self.const, self.const_index = bytearray(b), e["index"]
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

    def token(self, label):
        """The text token a menu label reads.

        The screen builds `bounce.screens.Menus.MainMenu` as a long run of
        `PUSHREGISTER 1; PUSHCONST <HAL_NAME>; PUSHCONST <TXT_NAME>;
        SETMEMBER`, and that run sits outside any function - it is the movie's
        own frame code - which is why it never appears in a listing of the
        screen's functions. It is the table that turns a menu entry's label
        into something the language database can answer.
        """
        want = self.constant(label)
        if want > 0xFF:
            return None
        for p in range(len(self.apt) - 6):
            if self.apt[p] != 0xA2 or self.apt[p + 1] != want:
                continue
            if self.apt[p + 2] == 0xA3 and self.apt[p + 5] == 0x4F:
                return self.constants[struct.unpack_from(">H", self.apt, p + 3)[0]]
            if self.apt[p + 2] == 0xA2 and self.apt[p + 4] == 0x4F:
                return self.constants[self.apt[p + 3]]
        return None

    def point_token(self, label, token):
        """Make a menu label read a different token.

        This edits the table itself - the `PUSHCONST` that supplies the token -
        rather than the string pool. Repointing a pool entry looks equivalent
        and is not: the pool's offsets run in ascending order, and a screen
        whose order is broken comes up with its text shifted, showing other
        screens' strings and raw token names. The operand is two bytes and
        changes nothing else.
        """
        want, to = self.constant(label), self.constant(token)
        if want > 0xFF:
            raise ValueError("%s is constant %d, too far for a byte operand"
                             % (label, want))
        for p in range(len(self.apt) - 6):
            if self.apt[p] != 0xA2 or self.apt[p + 1] != want:
                continue
            if self.apt[p + 2] == 0xA3 and self.apt[p + 5] == 0x4F:
                struct.pack_into(">H", self.apt, p + 3, to)
                return
        raise ValueError("%s is not in the screen's token table" % label)

    def save(self, path):
        self.archive.write(path, {self.apt_index: bytes(self.apt),
                                  self.const_index: bytes(self.const)})


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


def retag(scr, item, new):
    """Point an entry at a different label, and at the matching description.

    Both are read as `bounce.screens.Menus.MainMenu.<HAL_NAME>`, so switching
    one is a single operand byte - provided the constant is already in the
    screen's pool, which is the only kind of rename available without moving
    anything. `HAL_OPTIONS` is there because the sub-menu behind this entry
    already uses it.
    """
    for old, dest in ((item["label"], new),
                      (item["label"] + "_DESCRIPTION", new + "_DESCRIPTION")):
        want, to = scr.constant(old), scr.constant(dest)
        if to > 0xFF:
            raise ValueError("%s is constant %d, too far for a byte operand"
                             % (dest, to))
        for at, op, arg in item["ins"]:
            if op == 0xAF and arg == want:              # GETNAMEDMEMBER
                scr.apt[at + 1] = to
                break
        else:
            raise ValueError("%s does not read %s" % (item["label"], old))


def make_text(scr, item):
    """Turn an icon entry into a text one, in the same number of bytes.

    The icon row and the text list are the same array; what puts an entry in
    the row is the `IconAssetID` field on its object. Drop the field and the
    entry falls into the list above, drawn with its label instead of its
    picture. The four bytes that frees are padded out, so nothing moves.
    """
    n = item["end"] - item["start"]
    body = strip_icon(scr, item)
    scr.apt[item["start"]:item["end"]] = body + neutral_filler(n - len(body))


# The three Xbox Live entries in the icon row. All are dead: the services
# behind party sessions, leaderboards and achievements are gone.
DEAD_XBOX_LIVE = ("HAL_XBOX_LIVE_PARTY_SESSIONS", "HAL_LEADERBOARDS",
                  "HAL_ACHIEVEMENTS")

# The two that survive become text.
AS_TEXT = ("HAL_HELP_AND_OPTIONS", "HAL_EXIT")

# A row's label is a token, and the text behind it lives in
# data/xenon/loc/<language>.db. The two new rows want to read OPTIONS and EXIT
# GAME; as icons they carried "HELP & OPTIONS" and TXT_EXIT, and TXT_EXIT has
# no entry in the database at all - as an icon it never needed one, which is
# why the row first came up reading `*TXT_EXIT`.
#
# So Exit is pointed at a token that does have an entry: the one the removed
# party-sessions icon used, which nothing reads any more. Both tokens then get
# the words this menu wants, written into the database in place.
RETOKEN = (("HAL_EXIT", "TXT_XBOX_LIVE_PARTY_SESSIONS"),)
RETEXT = (("TXT_HELP_&_OPTIONS", "OPTIONS"),
          ("TXT_XBOX_LIVE_PARTY_SESSIONS", "EXIT GAME"))


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


def rebuild_menu(scr, drop=None, convert=None, retoken=None):
    """Make the main menu this port's: no dead icons, six rows of text.

    Each entry is a self-contained run of bytecode that builds an object and
    appends it to the menu array. Overwriting one with instructions that do
    nothing removes it; rewriting one in fewer bytes and padding the rest
    changes it. Either way every byte position in the file stays where it was.

    That matters more than it sounds. Removing the bytes outright and
    recomputing every position in the payload - which tools/apt_movie.py can
    do, and does correctly by every check available - still leaves the screen
    blank or faulting on a raw file offset. Something in the loader rebuilds
    pointers from the file in a way this does not yet account for. Until that
    is understood, an edit that moves nothing is the one that can be trusted,
    and it is enough for this job.
    """
    drop = list(DEAD_XBOX_LIVE if drop is None else drop)
    convert = list(AS_TEXT if convert is None else convert)
    retoken = list(RETOKEN if retoken is None else retoken)

    fn = menu_function(scr)
    items = {i["label"]: i for i in menu_items(scr, fn)[0]}
    missing = [n for n in drop + [c[0] if isinstance(c, tuple) else c
                                  for c in convert] if n not in items]
    if missing:
        raise SystemExit("this screen has no %s" % ", ".join(missing))

    log = []
    for entry in convert:
        label, rename = entry if isinstance(entry, tuple) else (entry, None)
        item = items[label]
        if not item["icon"]:
            raise SystemExit("%s is already a text entry" % label)
        if rename:
            retag(scr, item, rename)
        make_text(scr, item)
        log.append("%-30s icon -> text%s"
                   % (label, ", now reads %s" % rename if rename else ""))
    for label in drop:
        item = items[label]
        n = item["end"] - item["start"]
        scr.apt[item["start"]:item["end"]] = neutral_filler(n)
        log.append("%-30s removed (%d bytes, replaced in place)" % (label, n))
    for label, token in retoken:
        scr.point_token(label, token)
        log.append("%-30s now reads the token %s" % (label, token))

    scr.disassemble(fn["code"], fn["size"])     # must still decode cleanly
    return log, menu_items(scr, fn)[0]


def cmd_mainmenu(args):
    """rebuild_menu from the command line, and say what each row will read."""
    scr = Screen(args.source)
    convert = ([tuple((a.split("=", 1) + [None])[:2]) for a in args.totext]
               if args.totext else None)
    print("%s: menu builder at 0x%05X, %d bytes"
          % (os.path.basename(args.source), menu_function(scr)["at"],
             menu_function(scr)["size"]))
    log, after = rebuild_menu(scr, drop=args.drop, convert=convert,
                              retoken=[] if args.totext else None)
    for line in log:
        print("   " + line)

    db = locdb.LocDb(args.loc) if args.loc else None
    if db is not None and args.loc_dest:
        for token, text in RETEXT:
            was = db.get(token)
            db.rewrite(token, text)
            print("   %-30s %r -> %r" % (token, was, text))
        db.save(args.loc_dest)
        print("   wrote %s" % args.loc_dest)
    elif db is not None:
        for token, text in RETEXT:
            db.strings[token] = text            # show the intended result
    print("   menu is now:")
    for it in after:
        if it["array"] != 2:
            continue
        token = scr.token(it["label"])
        reads = ""
        if db is not None:
            text = db.get(token)
            reads = "  reads %r" % text if text else "  NO TEXT for %s" % token
        print("      %-24s %-5s%s" % (it["label"].replace("HAL_", ""),
                                      "icon" if it["icon"] else "text", reads))
    scr.save(args.dest)
    print("   wrote %s" % args.dest)
    return 0


def reshape_field(scr, name, width=None, height=None, size=None,
                  x=None, y=None, align=None):
    """Change the box a named text field lays its text out in.

    A screen's text is drawn inside a fixed rectangle, and a string longer
    than the rectangle simply stops being drawn - which is what turns a list
    of mods into four lines and nothing else. The width, height, alignment,
    point size and position are each a single number in the movie, so
    changing them moves nothing. Returns what the field was before, and where
    the two records that describe it live.
    """
    import apt_movie                          # imports this module in turn
    p = apt_movie.Payload(bytes(scr.apt))
    p.character(struct.unpack_from(">I", scr.const, 0x14)[0])
    body = apt_movie.text_field(p, name)
    place = apt_movie.placement(p, name)
    was = field_shape(scr, body, place)
    for value, at in ((width, body + apt_movie.TEXT_WIDTH),
                      (height, body + apt_movie.TEXT_HEIGHT),
                      (size, body + apt_movie.TEXT_SIZE),
                      (x, place + apt_movie.PLACE_X),
                      (y, place + apt_movie.PLACE_Y)):
        if value is not None:
            struct.pack_into(">f", scr.apt, at, value)
    if align is not None:
        struct.pack_into(">I", scr.apt, body + apt_movie.TEXT_ALIGN,
                         apt_movie.ALIGNMENTS[align])
    return was, body, place


def field_shape(scr, body, place):
    import apt_movie
    w, h = struct.unpack_from(">2f", scr.apt, body + apt_movie.TEXT_WIDTH)
    align = struct.unpack_from(">I", scr.apt, body + apt_movie.TEXT_ALIGN)[0]
    size = struct.unpack_from(">f", scr.apt, body + apt_movie.TEXT_SIZE)[0]
    x, y = struct.unpack_from(">2f", scr.apt, place + apt_movie.PLACE_X)
    names = {v: k for k, v in apt_movie.ALIGNMENTS.items()}
    return "%.0f x %.0f at (%.0f, %.0f), %s, %.0fpt" % (
        w, h, x, y, names.get(align, align), size)


def cmd_field(args):
    """Show a named text field, and reshape it.

    A screen's text is laid out in a fixed box, and a string longer than the
    box simply stops being drawn - which is what turns a list of mods into
    four lines and nothing else. Width, height, alignment, point size and
    position are all single numbers sitting in the movie, so changing them
    moves nothing.
    """
    scr = Screen(args.source)
    print("%s: %s" % (os.path.basename(args.source), args.name))
    was, body, place = reshape_field(
        scr, args.name, width=args.width, height=args.height, size=args.size,
        x=args.x, y=args.y, align=args.align)
    print("   now    %s" % was)
    now = field_shape(scr, body, place)
    if now == was:
        return 0
    print("   to     %s" % now)
    if not args.dest:
        raise SystemExit("give a destination to write the change")
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
    import apt_movie
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("list"); p.add_argument("source"); p.set_defaults(func=cmd_list)
    p = sub.add_parser("items"); p.add_argument("source"); p.set_defaults(func=cmd_items)
    p = sub.add_parser("mainmenu", help="rewrite the main menu in place")
    p.add_argument("source"); p.add_argument("dest")
    p.add_argument("--loc", metavar="PATH", help="a language database, to show "
                   "what each row will actually read")
    p.add_argument("--loc-dest", metavar="PATH", help="write the language "
                   "database the new rows need, alongside the screen")
    p.add_argument("--drop", action="append", metavar="HAL_NAME",
                   help="entry to remove (repeatable; defaults to the dead "
                        "Xbox Live trio)")
    p.add_argument("--totext", action="append", metavar="HAL_NAME[=HAL_NEW]",
                   help="entry to draw as text instead of an icon, optionally "
                        "relabelled (repeatable)")
    p.set_defaults(func=cmd_mainmenu)
    p = sub.add_parser("field", help="show or reshape a named text field")
    p.add_argument("source"); p.add_argument("name")
    p.add_argument("dest", nargs="?")
    p.add_argument("--width", type=float); p.add_argument("--height", type=float)
    p.add_argument("--size", type=float, help="point size")
    p.add_argument("--x", type=float); p.add_argument("--y", type=float)
    p.add_argument("--align", choices=sorted(apt_movie.ALIGNMENTS))
    p.set_defaults(func=cmd_field)
    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
