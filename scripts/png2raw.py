#!/usr/bin/env python3
# png2raw.py SRC.png OUT.raw WxH [--bg 0xRRGGBB]
#
# Convert a PNG to NanOS's flat 32bpp surface format so the compositor/toolkit can blit it without
# a PNG decoder. Output: little-endian [u32 width][u32 height][width*height u32 pixels], each pixel
# 0x00RRGGBB (the nw_surface format). Transparency is flattened over --bg (the surface has no
# alpha), so pass the background the image will sit on. Resized to WxH with a high-quality filter.
import sys, struct
from PIL import Image

src, out, wh = sys.argv[1], sys.argv[2], sys.argv[3]
bg = 0x000000
a = sys.argv[4:]
for i in range(0, len(a) - 1):
    if a[i] == "--bg":
        bg = int(a[i + 1], 0)
W, H = (int(v) for v in wh.lower().split("x"))
br, bgc, bb = (bg >> 16) & 255, (bg >> 8) & 255, bg & 255

im = Image.open(src).convert("RGBA").resize((W, H), Image.LANCZOS)
px = im.load()
buf = bytearray(struct.pack("<II", W, H))
for y in range(H):
    for x in range(W):
        r, g, b, al = px[x, y]
        R = (r * al + br * (255 - al)) // 255
        G = (g * al + bgc * (255 - al)) // 255
        B = (b * al + bb * (255 - al)) // 255
        buf += struct.pack("<I", (R << 16) | (G << 8) | B)
open(out, "wb").write(buf)
print(f"wrote {out}: {W}x{H} ({len(buf)} bytes)")
