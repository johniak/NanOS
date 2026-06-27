#!/usr/bin/env python3
# tools/gen-icons.py — generate the libnwui 48x48 icon set (NanOS-glass style). Stdlib only
# (no PIL/cairo), so it runs on the macOS host with zero deps.
#
# Icons are crisp (no anti-aliasing) on a MAGENTA color-key background (0xFF00FF). The toolkit's
# iconview (user/libnwui/nwui_paint.c, NWUI_ICON_KEY) skips key pixels when blitting, so the
# background and selection highlight show through — true transparency despite the decoder dropping
# the alpha channel. KEEP the key in sync with NWUI_ICON_KEY.
import zlib, struct, os

S = 48
KEY = (255, 0, 255)

def blank():
    return [[KEY for _ in range(S)] for _ in range(S)]

def px(b, x, y, c):
    if 0 <= x < S and 0 <= y < S:
        b[y][x] = c

def rect(b, x0, y0, x1, y1, c):
    for y in range(max(0, y0), min(S, y1)):
        for x in range(max(0, x0), min(S, x1)):
            b[y][x] = c

def frame(b, x0, y0, x1, y1, c):
    rect(b, x0, y0, x1, y0 + 1, c); rect(b, x0, y1 - 1, x1, y1, c)
    rect(b, x0, y0, x0 + 1, y1, c); rect(b, x1 - 1, y0, x1, y1, c)

def disc(b, cx, cy, r, c):
    for y in range(cy - r, cy + r + 1):
        for x in range(cx - r, cx + r + 1):
            if (x - cx) ** 2 + (y - cy) ** 2 <= r * r:
                px(b, x, y, c)

def png_rgb(b):
    raw = b"".join(b"\x00" + bytes(v for pxl in row for v in pxl) for row in b)
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", S, S, 8, 2, 0, 0, 0))  # 8-bit RGB
            + chunk(b"IDAT", zlib.compress(raw, 9))
            + chunk(b"IEND", b""))

def emit(name, b):
    os.makedirs("assets/icons", exist_ok=True)
    open("assets/icons/%s.png" % name, "wb").write(png_rgb(b))
    print("wrote assets/icons/%s.png" % name)

# palette
AMBER  = (240, 190, 80);  AMBER_D = (200, 150, 40)
GREY   = (150, 160, 172); GREY_D = (95, 105, 120)
DARK   = (40, 58, 88);    BLUE   = (70, 132, 205); BLUE_L = (120, 175, 235)
PAPER  = (236, 240, 247); INK    = (90, 100, 115)
GREEN  = (90, 170, 90);   GREEN_D = (60, 130, 60)
DOOR   = (110, 80, 50);   SUN    = (240, 200, 90)

def folder():
    b = blank()
    rect(b, 6, 14, 22, 19, AMBER_D)          # tab
    rect(b, 5, 18, 43, 38, AMBER)            # body
    frame(b, 5, 18, 43, 38, AMBER_D)
    return b

def drive():
    b = blank()
    rect(b, 7, 16, 41, 34, GREY); frame(b, 7, 16, 41, 34, GREY_D)
    rect(b, 11, 20, 37, 25, DARK)            # label slot
    disc(b, 35, 30, 2, GREEN)                # activity LED
    return b

def computer():
    b = blank()
    rect(b, 8, 9, 40, 33, DARK)              # monitor bezel
    rect(b, 11, 12, 37, 30, BLUE)            # screen
    rect(b, 11, 12, 37, 18, BLUE_L)          # screen sheen
    rect(b, 20, 33, 28, 39, GREY)            # neck
    rect(b, 14, 39, 34, 42, GREY_D)          # base
    return b

def home():
    b = blank()
    for y in range(10, 26):                  # roof triangle
        half = y - 10
        rect(b, 24 - half, y, 24 + half, y + 1, GREEN)
    rect(b, 12, 26, 36, 40, PAPER); frame(b, 12, 26, 36, 40, GREEN_D)
    rect(b, 21, 31, 27, 40, DOOR)            # door
    return b

def program():
    b = blank()
    rect(b, 8, 11, 40, 37, BLUE)             # window
    rect(b, 8, 11, 40, 17, DARK)             # title bar
    rect(b, 12, 22, 36, 25, PAPER)           # content lines
    rect(b, 12, 28, 30, 31, PAPER)
    return b

def text():
    b = blank()
    rect(b, 12, 8, 36, 40, PAPER); frame(b, 12, 8, 36, 40, GREY_D)
    for y in range(14, 38, 5):
        rect(b, 16, y, 32, y + 2, INK)
    return b

def image():
    b = blank()
    rect(b, 8, 12, 40, 36, PAPER); frame(b, 8, 12, 40, 36, GREY_D)
    disc(b, 17, 19, 3, SUN)                  # sun
    for x in range(10, 38):                  # mountains
        h = 30 - abs(x - 24) // 2
        rect(b, x, h, x + 1, 34, GREEN_D)
    return b

def generic():
    b = blank()
    rect(b, 13, 8, 35, 40, PAPER); frame(b, 13, 8, 35, 40, GREY_D)
    rect(b, 29, 8, 35, 14, GREY)             # folded corner
    return b

emit("folder", folder())
emit("drive", drive())
emit("computer", computer())
emit("home", home())
emit("program", program())
emit("text", text())
emit("image", image())
emit("file", generic())
