#!/usr/bin/env python3
"""
gen_icons.py -- generate the bw_* tool icons (examples/icons/*.ico).

One family, the station theme (bw_theme.h): dark rounded square, grayscale chrome,
electric blue + purple as the only accents. One glyph per tool:

  bw_layout_tool   ring of speaker dots, one highlighted (placing a speaker)
  bw_calib_view    magnitude curve over a baseline (the calibration report)
  bw_playground    headphones (binaural audition)
  bw_calibrate     sine sweep chirp
  bw_zylia_probe   mic sphere (capsule dots on a ball)
  bw_track_monitor crosshair + tracked dot
  bw_profile_bench ascending bars

Drawn at 1024 px (4x supersample), downscaled to 256, saved as .ico with
256/48/32/24/16 entries. The .ico files are committed so builds never need
Python/Pillow; re-run this only to change the design:

  python examples/icons/gen_icons.py     (writes next to this script; also
                                          preview_sheet.png, not committed)
"""
import os
from PIL import Image, ImageDraw

S, C = 1024, 512                      # supersampled canvas, its center
BG     = (24, 24, 27, 255)            # theme WindowBg #18181B
BORDER = (74, 74, 88, 255)            # subtle edge so the tile reads on dark taskbars
BLUE   = (61, 80, 238, 255)           # theme accent blue  #3D50EE
PURPLE = (158, 58, 208, 255)          # theme accent purple #9E3AD0
GRAY   = (150, 150, 162, 255)
YELLOW = (245, 220, 90, 255)          # the layout tool's "selected speaker"
WHITE  = (232, 232, 236, 255)

OUT = os.path.dirname(os.path.abspath(__file__))
ICO_SIZES = [(256, 256), (48, 48), (32, 32), (24, 24), (16, 16)]


def base():
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([64, 64, S - 64, S - 64], radius=200, fill=BG,
                        outline=BORDER, width=16)
    return img, d


def dot(d, x, y, r, color):
    d.ellipse([x - r, y - r, x + r, y + r], fill=color)


def ring_pts(n, radius, phase_deg=-90.0):
    import math
    return [(C + radius * math.cos(math.radians(phase_deg + i * 360.0 / n)),
             C + radius * math.sin(math.radians(phase_deg + i * 360.0 / n))) for i in range(n)]


def glyph_layout_tool(d):
    pts = ring_pts(8, 296)
    for i, (x, y) in enumerate(pts):
        dot(d, x, y, 96 if i == 1 else 80, YELLOW if i == 1 else BLUE)
    dot(d, C, C, 52, WHITE)                                   # the listener


def glyph_calib_view(d):
    import math
    d.line([(200, 700), (824, 700)], fill=GRAY, width=24)     # baseline (0 dB)
    pts = [(200 + t * 624 / 100,
            700 - 150 - 130 * math.sin(t / 100 * 2.2 * math.pi) * math.exp(-t / 120))
           for t in range(101)]
    d.line(pts, fill=BLUE, width=88, joint="curve")           # the correction curve
    dot(d, *pts[-1], 64, PURPLE)


def glyph_playground(d):
    d.arc([232, 232, 792, 792], start=180, end=360, fill=BLUE, width=96)   # headband
    d.rounded_rectangle([196, 480, 340, 724], radius=64, fill=PURPLE)      # ear cups
    d.rounded_rectangle([684, 480, 828, 724], radius=64, fill=PURPLE)


def glyph_calibrate(d):
    import math
    pts = []
    for t in range(161):
        u = t / 160.0
        phase = 2.0 * math.pi * (0.75 * u + 2.25 * u * u)     # chirp: frequency rises
        pts.append((176 + u * 672, C - 230 * math.sin(phase)))
    d.line(pts, fill=BLUE, width=88, joint="curve")
    dot(d, *pts[-1], 60, PURPLE)


def glyph_zylia_probe(d):
    d.ellipse([192, 192, 832, 832], outline=PURPLE, width=56)  # the sphere
    dot(d, C, C, 68, PURPLE)                                   # capsules
    for x, y in ring_pts(6, 200, phase_deg=-90):
        dot(d, x, y, 68, GRAY)


def glyph_track_monitor(d):
    d.ellipse([232, 232, 792, 792], outline=BLUE, width=64)    # crosshair ring
    for a, b in [((C, 96), (C, 264)), ((C, 760), (C, 928)),
                 ((96, C), (264, C)), ((760, C), (928, C))]:
        d.line([a, b], fill=BLUE, width=64)
    dot(d, C + 60, C - 60, 84, PURPLE)                         # the tracked head, off-center


def glyph_profile_bench(d):
    d.line([(200, 812), (824, 812)], fill=GRAY, width=24)
    for i, (h, col) in enumerate([(220, GRAY), (380, BLUE), (540, PURPLE)]):
        x = 264 + i * 216
        d.rounded_rectangle([x - 72, 812 - h, x + 72, 788], radius=40, fill=col)


TOOLS = {
    "bw_layout_tool":   glyph_layout_tool,
    "bw_calib_view":    glyph_calib_view,
    "bw_playground":    glyph_playground,
    "bw_calibrate":     glyph_calibrate,
    "bw_zylia_probe":   glyph_zylia_probe,
    "bw_track_monitor": glyph_track_monitor,
    "bw_profile_bench": glyph_profile_bench,
}


def main():
    previews = []
    for name, glyph in TOOLS.items():
        img, d = base()
        glyph(d)
        img256 = img.resize((256, 256), Image.LANCZOS)
        img256.save(os.path.join(OUT, name + ".ico"), format="ICO", sizes=ICO_SIZES)
        previews.append((name, img256))
        print("wrote", name + ".ico")

    # contact sheet (64 px + 16 px rows) to eyeball small-size legibility; not committed
    pad, cell = 16, 96
    sheet = Image.new("RGBA", (pad + len(previews) * (cell + pad), pad * 3 + 64 + 16), (40, 40, 46, 255))
    for i, (_, im) in enumerate(previews):
        x = pad + i * (cell + pad)
        i64, i16 = im.resize((64, 64), Image.LANCZOS), im.resize((16, 16), Image.LANCZOS)
        sheet.paste(i64, (x, pad), i64)                       # paste with alpha (transparent corners)
        sheet.paste(i16, (x + 24, pad * 2 + 64), i16)
    sheet.save(os.path.join(OUT, "preview_sheet.png"))
    print("wrote preview_sheet.png")


if __name__ == "__main__":
    main()
