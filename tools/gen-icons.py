#!/usr/bin/env python3
# tools/gen-icons.py — generate the libnwui 64x64 icon set (modern macOS-style). Stdlib only
# (no PIL/cairo), so it runs on the macOS host with zero deps.
#
# Icons sit on a MAGENTA color-key background (0xFF00FF); the toolkit iconview
# (user/libnwui/nwui_paint.c, NWUI_ICON_KEY) skips key pixels when blitting, so the cell + the
# selection highlight show through — true transparency despite the decoder dropping alpha. The
# silhouette is hard-edged (no AA against the key, so no magenta fringe); depth comes from BAKED
# vertical gradients + a top sheen band. Big colourful rounded "squircle" tiles with white glyphs,
# plus document-style icons. Drop shadow is drawn by the iconview, not baked. KEEP key==NWUI_ICON_KEY.
import zlib, struct, os

S = 64
KEY = (255, 0, 255)

def blank():
    return [[KEY for _ in range(S)] for _ in range(S)]

def lerp(a, b, t):
    return (int(a[0]+(b[0]-a[0])*t), int(a[1]+(b[1]-a[1])*t), int(a[2]+(b[2]-a[2])*t))

def px(b, x, y, c):
    if 0 <= x < S and 0 <= y < S: b[y][x] = c

def inside_round(x, y, x0, y0, x1, y1, r):
    if x < x0 or x >= x1 or y < y0 or y >= y1: return False
    if (x < x0+r or x >= x1-r) and (y < y0+r or y >= y1-r):
        ddx = (x-(x0+r-1)) if x < x0+r else (x-(x1-r))
        ddy = (y-(y0+r-1)) if y < y0+r else (y-(y1-r))
        return ddx*ddx+ddy*ddy <= r*r
    return True

def vgrad(b, x0, y0, x1, y1, top, bot, r=0):
    h = max(1, y1-y0)
    for y in range(max(0,y0), min(S,y1)):
        c = lerp(top, bot, (y-y0)/(h-1) if h > 1 else 0)
        for x in range(max(0,x0), min(S,x1)):
            if r and not inside_round(x, y, x0, y0, x1, y1, r): continue
            b[y][x] = c

def rect(b, x0, y0, x1, y1, c):
    for y in range(max(0,y0), min(S,y1)):
        for x in range(max(0,x0), min(S,x1)): b[y][x] = c

def sheen(b, x0, y0, x1, h, r):
    """A faint white top sheen band over a tile (alpha-ish via fixed light blend)."""
    for y in range(y0, y0+h):
        for x in range(x0, x1):
            if inside_round(x, y, x0, y0, x1, y0+200, r):
                cr, cg, cb = b[y][x]
                if (cr, cg, cb) != KEY:
                    b[y][x] = (min(255, cr+38), min(255, cg+38), min(255, cb+40))

def disc(b, cx, cy, r, c):
    for y in range(cy-r, cy+r+1):
        for x in range(cx-r, cx+r+1):
            if (x-cx)**2+(y-cy)**2 <= r*r: px(b, x, y, c)

def tri(b, pts, c):
    xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
    def area(a,bb,cc): return (bb[0]-a[0])*(cc[1]-a[1])-(bb[1]-a[1])*(cc[0]-a[0])
    for y in range(min(ys), max(ys)+1):
        for x in range(min(xs), max(xs)+1):
            d1=area(pts[0],pts[1],(x,y)); d2=area(pts[1],pts[2],(x,y)); d3=area(pts[2],pts[0],(x,y))
            if (d1>=0 and d2>=0 and d3>=0) or (d1<=0 and d2<=0 and d3<=0): px(b,x,y,c)

def png_rgb(b):
    raw = b"".join(b"\x00"+bytes(v for pxl in row for v in pxl) for row in b)
    def chunk(t,d): return struct.pack(">I",len(d))+t+d+struct.pack(">I",zlib.crc32(t+d)&0xffffffff)
    return (b"\x89PNG\r\n\x1a\n"+chunk(b"IHDR",struct.pack(">IIBBBBB",S,S,8,2,0,0,0))
            +chunk(b"IDAT",zlib.compress(raw,9))+chunk(b"IEND",b""))

def emit(name, b):
    os.makedirs("assets/icons", exist_ok=True)
    open("assets/icons/%s.png"%name,"wb").write(png_rgb(b)); print("wrote assets/icons/%s.png"%name)

WHITE=(255,255,255)
# gradient tile pairs
BLUE=((94,194,255),(31,140,255)); BLUEBACK=((154,214,255),(111,184,255))
GREY=((174,180,189),(124,132,146)); ORANGE=((255,157,92),(255,107,61))
INDIGO=((123,140,255),(91,76,255)); GREEN=((55,211,154),(19,168,119))
PAPERTOP=(255,255,255); PAPERBOT=(233,238,246); PAPEREDGE=(196,200,210); BLUESTRIP=(10,132,255)

def tile(b, grad, r=14, x0=6, y0=6, x1=58, y1=58):
    vgrad(b, x0, y0, x1, y1, grad[0], grad[1], r=r); sheen(b, x0, y0, x1, 14, r)

def folder():
    b = blank()
    # back tab
    rect(b, 7, 18, 30, 26, lerp(BLUEBACK[0], BLUEBACK[1], .3))
    vgrad(b, 6, 22, 58, 56, BLUE[0], BLUE[1], r=8); sheen(b, 6, 22, 58, 12, 8)
    return b

def drive():
    b = blank(); tile(b, GREY)
    rect(b, 16, 40, 48, 46, (90,98,112))         # slot
    disc(b, 45, 22, 3, (58,208,127))             # LED
    return b

def home():
    b = blank(); tile(b, ORANGE)
    tri(b, [(32,16),(50,32),(14,32)], WHITE)     # roof
    rect(b, 19, 31, 45, 48, WHITE)               # body
    rect(b, 28, 38, 36, 48, lerp(ORANGE[0],ORANGE[1],.5))  # door
    return b

def program():
    b = blank(); tile(b, INDIGO)
    tri(b, [(26,22),(46,32),(26,42)], WHITE)     # play glyph
    return b

def text():
    b = blank()
    vgrad(b, 16, 8, 48, 56, PAPERTOP, PAPERBOT, r=7)
    rect(b, 16, 8, 48, 16, BLUESTRIP)            # colored top
    for y in range(24, 48, 7): rect(b, 22, y, 42, y+3, (194,200,210))
    return b

def image():
    b = blank(); tile(b, GREEN)
    disc(b, 24, 26, 5, WHITE)                    # sun
    tri(b, [(14,48),(28,34),(40,48)], WHITE)     # mountains
    tri(b, [(34,48),(46,36),(54,48)], (236,255,244))
    return b

def generic():
    b = blank()
    vgrad(b, 16, 8, 48, 56, PAPERTOP, PAPERBOT, r=7)
    rect(b, 38, 8, 48, 18, (210,216,224))        # folded corner
    return b

def computer():
    b = blank(); tile(b, GREY)
    vgrad(b, 16, 18, 48, 38, BLUE[0], BLUE[1])   # screen
    rect(b, 27, 40, 37, 46, (96,104,118))        # stand
    return b

emit("folder", folder()); emit("drive", drive()); emit("computer", computer())
emit("home", home()); emit("program", program()); emit("text", text())
emit("image", image()); emit("file", generic())
