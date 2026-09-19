#!/usr/bin/env python3
"""Read the game's text: data/xenon/loc/<language>.db.

Every word the front end shows comes from here. A screen never holds its own
text - it holds a token like `TXT_EXIT_GAME`, and this database turns that into
"EXIT GAME". A menu label is therefore only ever as good as what is written
here, which is why the main menu's EXIT row first came up reading `*TXT_EXIT`:
the token was in the screen, but as an icon it had never needed a string, so
the database had nothing to answer with.

**Format.** It is a Tiburon `t3db`, the same container as Madden's, described
by the `-meta.xml` beside it: one table, `LanguageStrings`, three fields -
`hashid`, `stringid`, `sourcetext`. What the meta file does not say is how the
text is stored, which is:

    header, 0x20 bytes        "DB", the file size, the table count
    table header              field definitions, and at +0x34 the record count
                              stored twice (max, current)
    records, 16 bytes each    two byte offsets into the blob, then the hash
    blob                      a Huffman tree, then every string

The two offsets in a record are the string's *id* and its text, as positions
in the blob - which starts at the tree, so the smallest offset any string can
have is the tree's own length. Each string is a length and then Huffman codes,
most significant bit first, restarting on a byte boundary for each string.

How wide that length is comes from the field's declared depth in the meta
file: `stringid` holds 100 characters and takes one byte, `sourcetext` holds
4000 and takes two. Getting that wrong is invisible when reading - a short
text's two-byte length just looks like a zero and then a one-byte one - and
fatal when writing, because the game reads the width the field says and a
one-byte length there becomes a string thousands of characters long, running
on through the rest of the database.

The tree is an array of nodes, two big-endian 16-bit children each. A child
below 0x100 is a leaf holding that character; anything else is `index << 8`,
the node to walk to. Node 0 is the root, which is why it can never be a child
and why zero is unambiguously the character rather than the node.

**Writing.** A string can be replaced as long as the new one re-encodes into
the bytes the old one occupied, which shorter text almost always does. Nothing
moves, so no offset, no record and no hash has to change - the same rule that
makes the screens safe to edit. The tree is left alone too: it already codes
every character the game uses, and rebuilding it would move every string.

The file cannot grow. Appending a string and correcting both lengths in the
header - the file's size, and where the strings stop - gives a database the
game reads far enough to crash on, every time; whatever else it checks, a
longer file does not survive it. A record's offset, on the other hand, can be
aimed anywhere in the file, which is what LocDb.space uses to find room for
text that will not fit where it is.

Usage:
    python tools/locdb.py <eng_us.db>                 dump every string
    python tools/locdb.py <eng_us.db> TXT_EXIT_GAME   one token
    python tools/locdb.py <eng_us.db> --find "EXIT"   search the text
    python tools/locdb.py <in.db> --set TOKEN=TEXT --out <out.db>
"""
import argparse
import struct
import sys

# How many bytes each field spends saying how long its string is,
# from the depths the -meta.xml declares: 100 characters and 4000.
NAME_WIDTH = 1
TEXT_WIDTH = 2


class LocDb:
    """The strings in one language database, by token."""

    def __init__(self, path):
        self.path = path
        self.buf = bytearray(open(path, "rb").read())
        if self.buf[:2] != b"DB":
            raise ValueError("%s: not a t3db" % path)
        # One table, so its header follows the single table definition at 0x18.
        self.records = struct.unpack_from(">H", self.buf, 0x36)[0]
        self.base = 0x78 + 16 * self.records
        self._ends = None
        self.tree = self._tree()
        self.strings = self._read()
        self._codes = self._ends = None

    def _tree(self):
        """The Huffman nodes, which run from the blob's start to the first
        string - so the first record's smallest offset is the tree's size."""
        first = min(struct.unpack_from(">2I", self.buf, 0x78 + 16 * i)[0]
                    for i in range(self.records))
        return [struct.unpack_from(">HH", self.buf, self.base + i)
                for i in range(0, first, 4)]

    def _decode(self, off, count):
        out, bit = [], off * 8
        for _ in range(count):
            node = 0
            while True:
                byte = self.buf[self.base + (bit >> 3)]
                child = self.tree[node][(byte >> (7 - (bit & 7))) & 1]
                bit += 1
                if child >= 0x100 and not child & 0xFF:
                    node = child >> 8
                    continue
                out.append(chr(child))
                break
        return "".join(out)

    def _string(self, off, width):
        n = (self.buf[self.base + off] if width == 1 else
             struct.unpack_from(">H", self.buf, self.base + off)[0])
        return self._decode(off + width, n)

    def _read(self):
        out, end = {}, len(self.buf) - self.base
        for i in range(self.records):
            name, text, _hash, _pad = struct.unpack_from(
                ">4I", self.buf, 0x78 + 16 * i)
            if max(name, text) + 2 >= end:      # the tail record is a sentinel
                continue
            out[self._string(name, NAME_WIDTH)] = self._string(text, TEXT_WIDTH)
        return out

    # -- writing ---------------------------------------------------------
    def codes(self):
        """Every character the tree can code, and the bits that reach it."""
        if self._codes is None:
            self._codes = {}
            stack = [(0, "")]
            while stack:
                node, bits = stack.pop()
                for side, child in enumerate(self.tree[node]):
                    if child >= 0x100 and not child & 0xFF:
                        stack.append((child >> 8, bits + str(side)))
                    else:
                        self._codes[chr(child)] = bits + str(side)
        return self._codes

    def encode(self, text, width=TEXT_WIDTH):
        """A length and then the codes, padded to a whole number of bytes."""
        codes = self.codes()
        missing = sorted(set(text) - set(codes))
        if missing:
            raise ValueError("this database cannot write %s" % missing)
        if len(text) >= 1 << (8 * width):
            raise ValueError("%d characters is past what %d byte(s) can say"
                             % (len(text), width))
        bits = "".join(codes[c] for c in text)
        bits += "0" * (-len(bits) % 8)
        return len(text).to_bytes(width, "big") + bytes(
            int(bits[i:i + 8], 2) for i in range(0, len(bits), 8))

    def _extent(self):
        """Where each string ends: the next one along, since they are packed."""
        if self._ends is None:
            offs = set()
            for i in range(self.records):
                a, b, _h, _p = struct.unpack_from(">4I", self.buf, 0x78 + 16 * i)
                offs.update((a, b))
            order = sorted(o for o in offs if o + 2 < len(self.buf) - self.base)
            self._ends = dict(zip(order, order[1:] + [len(self.buf) - self.base]))
        return self._ends

    def _record(self, token):
        for i in range(self.records):
            name, at, _h, _p = struct.unpack_from(">4I", self.buf, 0x78 + 16 * i)
            if at + 2 < len(self.buf) - self.base and \
                    self._string(name, NAME_WIDTH) == token:
                return 0x78 + 16 * i, at
        raise KeyError("%s is not in %s" % (token, self.path))

    def rewrite(self, token, text):
        """Put `text` where `token`'s text is, if it fits in the same bytes."""
        _rec, at = self._record(token)
        new, room = self.encode(text), self._extent()[at] - at
        if len(new) > room:
            raise ValueError("%r needs %d bytes, %s has %d"
                             % (text, len(new), token, room))
        self.buf[self.base + at:self.base + at + len(new)] = new
        self.strings[token] = text

    def _spans(self):
        """Every string in the blob, in the order they are stored."""
        out = []
        for i in range(self.records):
            name, text, _h, _p = struct.unpack_from(">4I", self.buf, 0x78 + 16 * i)
            if max(name, text) + 2 >= len(self.buf) - self.base:
                continue
            rec = 0x78 + 16 * i
            for field, off, width in ((0, name, NAME_WIDTH), (4, text, TEXT_WIDTH)):
                s = self._string(off, width)
                out.append({"at": off, "len": len(self.encode(s, width)),
                            "rec": rec, "field": field, "token":
                            self._string(name, NAME_WIDTH), "width": width})
        out.sort(key=lambda s: s["at"])
        return out

    def space(self, want, expendable):
        """Find `want` bytes of blob to reuse, taken from text that cannot show.

        Nothing can be added to this database. A longer file is refused at
        load, and the strings are packed end to end with not one byte spare.
        What there is instead is a great deal of text an offline build can
        never reach - an online SDK's worth of lobbies, clubs and invitations
        - and a run of those, adjacent in the blob, is room.

        The records that lose their text are not left pointing into the middle
        of whatever is written over it. Each is aimed at a short string that is
        still intact - itself one of the expendable ones, so that no live
        token ends up with a second record claiming its name.
        """
        spans = self._spans()
        run, total = [], 0
        for s in spans:
            if not expendable(s["token"]):
                run, total = [], 0
                continue
            if run and s["at"] != run[-1]["at"] + run[-1]["len"]:
                run, total = [], 0                  # a gap means a separate run
                continue
            run.append(s)
            total += s["len"]
            if total >= want:
                break
        else:
            raise ValueError("no run of %d expendable bytes in %s"
                             % (want, self.path))

        taken = {id(s) for s in run}
        stand = {}
        for w in (NAME_WIDTH, TEXT_WIDTH):
            spare = [s for s in spans if s["width"] == w
                     and expendable(s["token"]) and id(s) not in taken]
            if not spare:
                raise ValueError("nothing expendable left to stand in for the "
                                 "%d bytes taken" % total)
            stand[w] = min(spare, key=lambda s: s["len"])["at"]
        for s in run:
            struct.pack_into(">I", self.buf, s["rec"] + s["field"],
                             stand[s["width"]])
            self.strings.pop(s["token"], None)
        self._ends = None
        return run[0]["at"], total

    def set(self, token, text, expendable=None):
        """Replace a token's text, borrowing space only if it will not fit."""
        try:
            self.rewrite(token, text)
            return False
        except ValueError:
            if expendable is None:
                raise
        new = self.encode(text)
        at, _room = self.space(len(new), expendable)
        self.buf[self.base + at:self.base + at + len(new)] = new
        rec, _old = self._record(token)
        struct.pack_into(">I", self.buf, rec + 4, at)
        self.strings[token] = text
        return True

    def save(self, path):
        open(path, "wb").write(bytes(self.buf))

    def get(self, token, default=None):
        return self.strings.get(token, default)

    def __contains__(self, token):
        return token in self.strings

    def __len__(self):
        return len(self.strings)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source")
    ap.add_argument("token", nargs="*")
    ap.add_argument("--find", metavar="TEXT", help="tokens whose text contains this")
    ap.add_argument("--set", action="append", metavar="TOKEN=TEXT", default=[],
                    help="replace a string, in the bytes it already has")
    ap.add_argument("--out", metavar="PATH", help="where to write --set changes")
    args = ap.parse_args(argv)

    db = LocDb(args.source)
    if args.set:
        if not args.out:
            raise SystemExit("--set needs --out")
        for pair in args.set:
            token, _, text = pair.partition("=")
            was = db.get(token)
            db.rewrite(token, text)
            print("%-44s %r -> %r" % (token, was, text))
        db.save(args.out)
        print("wrote %s" % args.out)
        return 0
    if args.token:
        for t in args.token:
            print("%-44s %r" % (t, db.get(t, "<not in this database>")))
        return 0
    want = (args.find or "").lower()
    for k in sorted(db.strings):
        if want and want not in db.strings[k].lower():
            continue
        print("%-44s %r" % (k, db.strings[k]))
    if not want:
        print("\n%d strings" % len(db))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
