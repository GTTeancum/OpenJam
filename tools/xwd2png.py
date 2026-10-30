# Converts a 32-bit X11 window dump to PNG, reading the pixels as 0x00RRGGBB.
# xwdtopnm mixes up the channels for the DirectColor visual the native build's
# window gets under WSLg, so this reads them directly.
import struct, sys
from PIL import Image
d = open(sys.argv[1], "rb").read()
h = struct.unpack(">25I", d[:100])
header_size, w, hgt, bpl, ncolors = h[0], h[4], h[5], h[12], h[19]
data = header_size + 12 * ncolors
img = Image.frombuffer("RGBX", (w, hgt), d[data:data + bpl * hgt], "raw", "BGRX", bpl, 1)
img.convert("RGB").save(sys.argv[2])
