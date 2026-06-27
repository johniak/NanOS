#!/usr/bin/env python3
# tools/gen-icons.py — render the NanOS icon set from SVG to transparent PNG (real vector art, AA,
# alpha), on the macOS host. Source SVGs: tools/icons.html (file-type tiles, 64x64) and
# tools/toolbar-icons.html (toolbar glyphs, 24x24). Rendered with headless Chrome (transparent
# background) and sliced with PIL into assets/icons/*.png. The PNG decoder (user/libnwui/nwui_png.c)
# preserves alpha and the iconview/iconbtn alpha-composite, so edges + rounded corners are clean.
#
# Run:  python3 tools/gen-icons.py     (needs Google Chrome + PIL)
import subprocess, sys, os
from PIL import Image

CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"

def render(html, w, h, out_png):
    subprocess.run([CHROME, "--headless", "--disable-gpu", "--force-device-scale-factor=1",
                    "--window-size=%d,%d" % (w, h), "--default-background-color=00000000",
                    "--hide-scrollbars", "--screenshot=" + out_png, html],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def slice_sheet(png, cell, names):
    im = Image.open(png).convert("RGBA")
    os.makedirs("assets/icons", exist_ok=True)
    for i, n in enumerate(names):
        im.crop((i * cell, 0, i * cell + cell, cell)).save("assets/icons/%s.png" % n)
        print("wrote assets/icons/%s.png" % n)

# file-type tiles (order matches the <svg> sequence in tools/icons.html)
render("tools/icons.html", 8 * 64, 64, "/tmp/_iconsheet.png")
slice_sheet("/tmp/_iconsheet.png", 64,
            ["folder", "drive", "home", "program", "text", "image", "file", "computer"])

# toolbar glyphs (order matches tools/toolbar-icons.html)
render("tools/toolbar-icons.html", 4 * 24, 24, "/tmp/_tbsheet.png")
slice_sheet("/tmp/_tbsheet.png", 24, ["ui-back", "ui-fwd", "ui-up", "ui-home"])
