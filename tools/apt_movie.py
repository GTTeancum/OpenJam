#!/usr/bin/env python3
"""Structural reader for the `Apt Data` payload inside a front-end `.ast`.

This is the layer above tools/apt.py. That one decodes ActionScript; this one
decodes the movie the ActionScript lives in - characters, frames, frame items,
imports and exports - so that the payload can be rebuilt with every internal
position recomputed rather than patched around.

The layout is OpenSAGE's, which documents the same format for the PC SAGE
titles: https://opensage.readthedocs.io/file-formats/apt/ and the reader
classes under OpenSage.Game/Data/Apt. Three details confirm it is the right
spec for this game's files: a character record starts with a type and the
signature 0x09876543, every list is a count followed by an absolute position,
and this screen's five empty character slots are filled by exactly the five
entries in its import table.

Two differences from the documented layout, both consistent across every
character in the file, and expected since these payloads say `Apt Data:1:7:4`
while OpenSAGE targets the PC titles:

  - a character body begins with two words that the documented order does not
    have; they are zero everywhere seen so far
  - everything else follows in the documented order

Coverage is the check that the model is complete. Every byte the parser
consumes is marked, so anything left unmarked is something this file does not
yet understand - and a rebuild cannot be trusted until that is near zero.
"""

import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from apt import Screen, MAGIC, ALIGNED, U32, U32X2, S32, U16, U8, OPCODES

CHARACTER_SIGNATURE = 0x09876543

CHARACTER_TYPES = {
    1: "Shape", 2: "Text", 3: "Font", 4: "Button", 5: "Sprite", 6: "Sound",
    7: "Image", 8: "Morph", 9: "Movie", 10: "StaticText", 11: "None", 12: "Video",
}

FRAME_ITEM_TYPES = {
    1: "Action", 2: "FrameLabel", 3: "PlaceObject", 4: "RemoveObject",
    5: "BackgroundColor", 8: "InitAction",
}

# PlaceObject carries a trailing position only when it has a clip action.
PLACE_OBJECT_HAS_CLIP_ACTION = 0x80


class Payload:
    def __init__(self, blob):
        self.buf = blob
        self.seen = bytearray(len(blob))
        self.notes = []
        self.done = {}
        self.pointers = {}
        self.sizes = []   # (field holding a code length, the block it belongs to)

    # -- primitives ------------------------------------------------------
    def mark(self, start, length, what):
        for i in range(start, min(start + length, len(self.buf))):
            self.seen[i] = 1
        self.notes.append((start, length, what))

    def u32(self, off):
        return struct.unpack_from(">I", self.buf, off)[0]

    def i32(self, off):
        return struct.unpack_from(">i", self.buf, off)[0]

    def pointer(self, field):
        """Note that the word at `field` holds a position, and return it."""
        target = self.u32(field)
        if target:
            self.pointers[field] = target
        return target

    def string(self, off):
        if not off or off >= len(self.buf):
            return ""
        end = self.buf.index(b"\0", off)
        self.mark(off, end - off + 1, "string")
        return self.buf[off:end].decode("latin-1")

    def listing(self, off, what):
        """A count followed by the position of the items."""
        count, at = self.u32(off), self.pointer(off + 4)
        self.notes.append((off, 8, "list:" + what))
        return count, at

    # -- structures ------------------------------------------------------
    def character(self, off, depth=0):
        # Slot 0 of a movie's character table points back at the movie, so
        # anything that walks the table has to remember where it has been.
        if off in self.done:
            return self.done[off]
        self.done[off] = {"offset": off, "type": "(being read)", "size": 0}
        kind, sig = self.u32(off), self.u32(off + 4)
        if sig != CHARACTER_SIGNATURE:
            raise ValueError("no character at 0x%X (signature 0x%08X)" % (off, sig))
        name = CHARACTER_TYPES.get(kind, "type%d" % kind)
        self.mark(off, 8, "character header (%s)" % name)
        # Every character body opens with the same two words, whatever the
        # type. The documented order does not have them; this version does.
        self.mark(off + 8, 8, "character lead-in")
        body = off + 16
        handler = getattr(self, "_" + name.lower(), None)
        size = 8 + (handler(body, depth) if handler else 0)
        self.mark(body, size, "%s body" % name)
        rec = {"offset": off, "type": name, "size": 8 + size}
        self.done[off] = rec
        return rec

    def _movie(self, off, depth):
        n, at = self.listing(off, "frames")
        for i in range(n):
            self.frame(at + i * 8)
        self.mark(off + 8, 4, "movie unknown")
        n, at = self.listing(off + 12, "characters")
        self.mark(at, n * 4, "character pointer table")
        for i in range(n):
            p = self.pointer(at + i * 4)
            if p:                      # empty slots are filled by imports
                self.character(p, depth + 1)
        self.mark(off + 20, 12, "screen size and frame time")
        n, at = self.listing(off + 32, "imports")
        for i in range(n):
            r = at + i * 16
            self.mark(r, 16, "import")
            self.string(self.pointer(r)), self.string(self.pointer(r + 4))
        n, at = self.listing(off + 40, "exports")
        for i in range(n):
            r = at + i * 8
            self.mark(r, 8, "export")
            self.string(self.pointer(r))
        self.mark(off + 48, 4, "playable tail")
        return 52

    def _sprite(self, off, depth):
        n, at = self.listing(off, "frames")
        for i in range(n):
            self.frame(at + i * 8)
        self.mark(off + 8, 4, "playable tail")
        return 12

    def frame(self, off):
        n, at = self.listing(off, "frame items")
        self.mark(at, n * 4, "frame item pointer table")
        for i in range(n):
            self.frame_item(self.pointer(at + i * 4))

    def frame_item(self, off):
        kind = self.u32(off)
        name = FRAME_ITEM_TYPES.get(kind, "type%d" % kind)
        self.mark(off, 4, "frame item (%s)" % name)
        body = off + 4
        if name == "Action":
            self.mark(body, 4, "action")
            self.instructions(self.pointer(body))
        elif name == "InitAction":
            self.mark(body, 8, "init action")
            self.instructions(self.pointer(body + 4))
        elif name == "FrameLabel":
            self.mark(body, 12, "frame label")
            self.string(self.pointer(body))
        elif name == "RemoveObject":
            self.mark(body, 4, "remove object")
        elif name == "BackgroundColor":
            self.mark(body, 4, "background colour")
        elif name == "PlaceObject":
            flags = self.u32(body)
            size = 60 if not (flags & PLACE_OBJECT_HAS_CLIP_ACTION) else 64
            self.mark(body, size, "place object")
            self.string(self.pointer(body + 44))
        else:
            self.notes.append((off, 4, "UNKNOWN frame item %d" % kind))

    def _text(self, off, depth):
        self.mark(off, 52, "text")
        self.string(self.pointer(off + 44))
        self.string(self.pointer(off + 48))
        return 52

    def _font(self, off, depth):
        self.string(self.pointer(off))
        n, at = self.listing(off + 4, "glyphs")
        self.mark(at, n * 4, "glyphs")
        return 12

    def _image(self, off, depth):
        return 4

    def _shape(self, off, depth):
        return 20

    # -- bytecode --------------------------------------------------------
    def instructions(self, start, stop=None):
        """Walk a block: to its terminator, or to `stop` for a function body."""
        if not start or start >= len(self.buf):
            return
        p = start
        limit = len(self.buf) if stop is None else stop
        while p < limit:
            op, at = self.buf[p], p
            p += 1
            if op in ALIGNED:
                p = (p + 3) & ~3
            if op in (0x8E, 0x9B):
                _n, _np, _f, _pp, size = struct.unpack_from(">5I", self.buf, p)
                # a function header names itself and its parameters by position
                self.pointer(p)
                self.pointer(p + 12)
                p += 20 + 8 + size
                continue
            if op == 0x88:                      # CONSTANTPOOL: count, position
                self.pointer(p + 4)
            if op in U32X2:
                p += 8
            elif op in U32 or op in S32:
                p += 4
            elif op in U16:
                p += 2
            elif op in U8:
                p += 1
            if op == 0x00 and stop is None:     # END, for a top-level block
                break
        self.mark(start, p - start, "instructions")

    def function_headers(self):
        """Every function header in the payload, found by its sentinel.

        Walking the instruction stream finds the ones at the top level, but a
        function defined inside another is only reached by decoding that body,
        and a stray 0x8E in the middle of an operand makes that unreliable. The
        sentinel is unambiguous, so all 39 in this screen are found the same
        way tools/apt.py finds them.
        """
        found, pos = 0, 0
        while True:
            m = self.buf.find(MAGIC, pos)
            if m < 0:
                return found
            pos = m + 8
            head = m - 20
            if head < 1:
                continue
            op = head - 1
            while op > 0 and self.buf[op] == 0:
                op -= 1
            if self.buf[op] not in (0x8E, 0x9B):
                continue
            self.pointer(head)          # the function's name
            params = self.pointer(head + 12)
            self.sizes.append((head + 16, m + 8))
            # The parameter list is pairs of (register, name), and the name is
            # a position of its own. Missing these leaves them pointing at
            # wherever the old text used to be, which is the sort of thing a
            # byte-for-byte diff cannot catch: the bytes are untouched, and
            # untouched is exactly what is wrong once everything after a cut
            # has moved.
            nparams = struct.unpack_from(">I", self.buf, head + 4)[0]
            if params and nparams and nparams < 64:
                for k in range(nparams):
                    self.pointer(params + 8 * k + 4)
            found += 1

    # -- report ----------------------------------------------------------
    def coverage(self):
        seen = sum(self.seen)
        gaps, run = [], None
        for i, v in enumerate(self.seen):
            if not v and run is None:
                run = i
            elif v and run is not None:
                gaps.append((run, i - run)); run = None
        if run is not None:
            gaps.append((run, len(self.seen) - run))
        return seen, len(self.buf), gaps



def rebuild(p, cuts):
    """Return the payload with `cuts` removed and every position corrected.

    Nothing is re-serialised. Each surviving byte keeps its order, so a rebuild
    with no cuts is the original file exactly - which is the point: the only
    thing that changes is what was asked for, plus the positions that have to
    follow it.
    """
    cuts = sorted(cuts)
    for (a, b), (c, d) in zip(cuts, cuts[1:]):
        if b > c:
            raise ValueError("overlapping cuts")

    def moved(old):
        """Where a byte ends up, or None if it was cut."""
        shift = 0
        for a, b in cuts:
            if old >= b:
                shift += b - a
            elif old >= a:
                return None
        return old - shift

    for field, target in p.pointers.items():
        if moved(field) is None:
            continue
        if moved(target) is None:
            raise ValueError("0x%05X still points into a cut, at 0x%05X" % (field, target))

    out = bytearray()
    prev = 0
    for a, b in cuts:
        out += p.buf[prev:a]
        prev = b
    out += p.buf[prev:]

    # positions first
    for field, target in p.pointers.items():
        nf, nt = moved(field), moved(target)
        if nf is not None:
            struct.pack_into(">I", out, nf, nt)

    # then any declared code length that lost bytes from inside it
    for field, block in p.sizes:
        nf = moved(field)
        if nf is None:
            continue
        size = p.u32(field)
        body = field + 4 + 8            # past the length and the sentinel
        lost = sum(min(b, body + size) - max(a, body)
                   for a, b in cuts if a < body + size and b > body)
        if lost:
            struct.pack_into(">I", out, nf, size - lost)
    return bytes(out)


def load(path):
    s = Screen(path)
    entry = struct.unpack_from(">I", s.const, 0x14)[0]
    return Payload(bytes(s.apt)), entry


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source")
    ap.add_argument("--gaps", type=int, default=12)
    args = ap.parse_args(argv)

    p, entry = load(args.source)
    root = p.character(entry)
    p.function_headers()
    seen, total, gaps = p.coverage()
    print("%s" % os.path.basename(args.source))
    print("  movie at 0x%05X, %s" % (entry, root["type"]))
    print("  understood %d of %d bytes (%.1f%%), %d gap(s)"
          % (seen, total, 100.0 * seen / total, len(gaps)))
    biggest = sorted(gaps, key=lambda g: -g[1])[:args.gaps]
    for at, n in biggest:
        head = p.buf[at:at + 12]
        print("     0x%05X  %5d bytes  %s" % (at, n, head.hex(" ")))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
