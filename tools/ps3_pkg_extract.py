#!/usr/bin/env python3
"""Extract a PlayStation 3 .pkg into a plain directory tree.

The NBA JAM: On Fire Edition mods are distributed as PS3 packages because that
is what a PS3 installs. The payload inside is an ordinary USRDIR tree, which is
what we actually want.

Format (all big-endian):

    0x00  magic 0x7F504B47 ".PKG"
    0x04  revision u16, type u16      (0x8000 retail, type 1 = PS3)
    0x14  item_count u32
    0x20  data_offset u64
    0x28  data_size u64
    0x30  content id, 0x24 bytes
    0x70  data riv, 16 bytes          - the AES-CTR starting counter

The data region is AES-128-CTR. Inside it, `item_count` records of 0x20 bytes
each give filename offset/size, data offset/size and a type in the low byte of
the flags.

Usage:
    python pkg_extract.py <file.pkg> <output_dir>
"""

import ctypes
import ctypes.util
import os
import struct
import sys

try:
    from Crypto.Cipher import AES
    from Crypto.Util import Counter
except ImportError:
    # Not on most Linux machines or a Steam Deck, and nothing should have to
    # be installed to add a mod there. The system's own OpenSSL library is
    # everywhere and does AES-CTR just as well; see _LibcryptoCtr.
    AES = Counter = None

# The PS3 package key. Published in the PS3 file format documentation and
# present in every open source PS3 package tool.
PS3_PKG_KEY = bytes.fromhex("2E7B71D7C9C9A14EA3221F188828B8F8")

ENTRY_TYPES = {
    0x01: "npdrm",
    0x02: "npdrm-edat",
    0x03: "file",
    0x04: "directory",
    0x06: "file",
    0x09: "file",
    0x0B: "file",
    0x0C: "file",
    0x0E: "file",
    0x0F: "file",
    0x10: "file",
    0x11: "file",
    0x12: "file",
}

CHUNK = 8 * 1024 * 1024


class _LibcryptoCtr:
    """AES-128-CTR from the system's libcrypto, shaped like a PyCryptodome
    cipher: `decrypt(data)` and nothing else. OpenSSL counts the counter as
    one 128-bit big-endian number, as the package format does."""

    _lib = None

    @classmethod
    def lib(cls):
        if cls._lib is None:
            names = [ctypes.util.find_library("crypto"), "libcrypto.so.3",
                     "libcrypto.so.1.1", "libcrypto.so"]
            for name in filter(None, names):
                try:
                    lib = ctypes.CDLL(name)
                    break
                except OSError:
                    continue
            else:
                raise RuntimeError(
                    "opening a PS3 package needs either the pycryptodome "
                    "Python package or the system's OpenSSL library, and "
                    "neither is here")
            lib.EVP_CIPHER_CTX_new.restype = ctypes.c_void_p
            lib.EVP_aes_128_ctr.restype = ctypes.c_void_p
            lib.EVP_CIPHER_CTX_free.argtypes = [ctypes.c_void_p]
            lib.EVP_DecryptInit_ex.argtypes = [
                ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
                ctypes.c_char_p, ctypes.c_char_p]
            lib.EVP_DecryptUpdate.argtypes = [
                ctypes.c_void_p, ctypes.c_char_p,
                ctypes.POINTER(ctypes.c_int), ctypes.c_char_p, ctypes.c_int]
            cls._lib = lib
        return cls._lib

    def __init__(self, key, counter):
        lib = self.lib()
        self.ctx = lib.EVP_CIPHER_CTX_new()
        iv = counter.to_bytes(16, "big")
        if not lib.EVP_DecryptInit_ex(self.ctx, lib.EVP_aes_128_ctr(), None,
                                      key, iv):
            raise RuntimeError("OpenSSL would not set up AES-CTR")

    def decrypt(self, data):
        out = ctypes.create_string_buffer(len(data) + 16)
        n = ctypes.c_int(0)
        if not self.lib().EVP_DecryptUpdate(self.ctx, out, ctypes.byref(n),
                                            bytes(data), len(data)):
            raise RuntimeError("OpenSSL could not decrypt")
        return out.raw[:n.value]

    def __del__(self):
        if self._lib is not None and getattr(self, "ctx", None):
            self._lib.EVP_CIPHER_CTX_free(self.ctx)
            self.ctx = None


def ctr_cipher(riv, block_offset=0):
    """A fresh AES-CTR cipher positioned `block_offset` 16-byte blocks in."""
    start = int.from_bytes(riv, "big")
    initial = (start + block_offset) % (1 << 128)
    if AES is None:
        return _LibcryptoCtr(PS3_PKG_KEY, initial)
    counter = Counter.new(128, initial_value=initial)
    return AES.new(PS3_PKG_KEY, AES.MODE_CTR, counter=counter)


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    pkg_path, out_dir = argv

    with open(pkg_path, "rb") as f:
        head = f.read(0x80)
        if head[:4] != b"\x7fPKG":
            print("not a PS3 package: bad magic")
            return 1
        revision, ptype = struct.unpack_from(">HH", head, 4)
        item_count = struct.unpack_from(">I", head, 0x14)[0]
        data_offset, data_size = struct.unpack_from(">QQ", head, 0x20)
        content_id = head[0x30:0x54].split(b"\0")[0].decode("ascii", "replace")
        riv = head[0x70:0x80]

        print("content id : {}".format(content_id))
        print("revision   : 0x{:04X}  type {}".format(revision, ptype))
        print("items      : {}".format(item_count))
        print("data       : offset 0x{:X}, {:,} bytes".format(data_offset, data_size))

        # The item table sits at the start of the data region. Decrypt just
        # enough of it to read every record and every name.
        table_bytes = item_count * 0x20
        f.seek(data_offset)
        # Names follow the table; 256 KB past it covers them comfortably.
        head_len = min(data_size, table_bytes + 256 * 1024)
        head_len = (head_len + 15) & ~15
        header_region = ctr_cipher(riv).decrypt(f.read(head_len))

        entries = []
        for i in range(item_count):
            off = i * 0x20
            (name_off, name_len, file_off, file_size, flags, _) = struct.unpack_from(
                ">IIQQII", header_region, off)
            kind = ENTRY_TYPES.get(flags & 0xFF, "file")
            if name_off + name_len <= len(header_region):
                name = header_region[name_off:name_off + name_len].rstrip(b"\0")
                name = name.decode("utf-8", "replace")
            else:
                name = "<name at 0x{:X} beyond decrypted window>".format(name_off)
            entries.append((name, kind, file_off, file_size))

        dirs = sum(1 for e in entries if e[1] == "directory")
        print("           : {} files, {} directories\n".format(len(entries) - dirs, dirs))

        os.makedirs(out_dir, exist_ok=True)
        written = 0
        for name, kind, file_off, file_size in entries:
            safe = name.replace("\\", "/").lstrip("/")
            target = os.path.join(out_dir, *[p for p in safe.split("/") if p not in ("", ".", "..")])
            if kind == "directory":
                os.makedirs(target, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(target) or ".", exist_ok=True)

            # Each file starts at a 16-byte aligned offset within the data
            # region, so the cipher can be positioned directly at its block
            # rather than streamed from the beginning.
            assert file_off % 16 == 0, "unaligned file offset 0x{:X}".format(file_off)
            cipher = ctr_cipher(riv, file_off // 16)
            f.seek(data_offset + file_off)
            remaining = file_size
            with open(target, "wb") as out:
                while remaining > 0:
                    n = min(CHUNK, remaining)
                    # AES-CTR needs whole blocks; read up to the block boundary
                    # and trim the plaintext afterwards.
                    padded = (n + 15) & ~15
                    data = f.read(padded)
                    out.write(cipher.decrypt(data)[:n])
                    remaining -= n
            written += 1
            if written % 50 == 0:
                print("  {} files...".format(written))

        print("\nextracted {} files to {}".format(written, out_dir))
    return 0


def extract(pkg_path, out_dir):
    """Open a package from another program. Raises when it cannot."""
    code = main([str(pkg_path), str(out_dir)])
    if code:
        raise RuntimeError("could not open {}".format(pkg_path))
    return out_dir


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
