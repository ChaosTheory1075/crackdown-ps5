#!/usr/bin/env python3
"""Draws the PlayStation button glyphs that replace the game's Xbox 360 ones (src/ps5/cd_glyphs.cpp).

    make_ps_button_glyphs.py [out dir]     (default: package/glyphs)

ps_button_glyphs.dds: 40 cells of 128x128 in 8 columns, uncompressed A8R8G8B8 with straight alpha, cell n for the
font's button character U+0190 + n (the order of the game's font: A B X Y LIVE - - START BACK LB RB LT RT, the two
sticks, the left stick's eight directions, the right stick's, the D-pad's, the D-pad). A fully transparent cell keeps
the game's own glyph (LIVE and the two unused ones).
ps_cross_40.png, ps_circle_40.png: the menus' A.png and B.png (40x40) as Cross and Circle.
preview.png: the sheet on grey.

Drawn here from shapes (and Windows' Arial Bold for the L1/R1/L2/R2 and stick labels); the generated files are
committed, so the build needs none of this.
"""
import math
import os
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

CELL = 128
COLUMNS = 8
COUNT = 40
SS = 4  # supersampling

OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "package", "glyphs")
FONT = "C:/Windows/Fonts/arialbd.ttf"

DARK = (26, 27, 33, 255)
RIM = (236, 236, 240, 255)
DIM = (92, 94, 104, 255)
WHITE = (246, 246, 248, 255)
CROSS = (124, 178, 232, 255)
CIRCLE = (255, 102, 102, 255)
SQUARE = (232, 140, 214, 255)
TRIANGLE = (64, 226, 160, 255)


def canvas():
    im = Image.new("RGBA", (CELL * SS, CELL * SS), (0, 0, 0, 0))
    return im, ImageDraw.Draw(im)


def s(v):
    return v * SS


def done(im):
    return im.resize((CELL, CELL), Image.LANCZOS)


def font(size):
    return ImageFont.truetype(FONT, s(size))


def disc(d, r=58, rim=5):
    c = s(64)
    d.ellipse([c - s(r), c - s(r), c + s(r), c + s(r)], fill=RIM)
    d.ellipse([c - s(r - rim), c - s(r - rim), c + s(r - rim), c + s(r - rim)], fill=DARK)


def face(symbol):
    im, d = canvas()
    disc(d)
    c, w = s(64), s(9)
    if symbol == "cross":
        k = s(24)
        d.line([c - k, c - k, c + k, c + k], fill=CROSS, width=w)
        d.line([c - k, c + k, c + k, c - k], fill=CROSS, width=w)
    elif symbol == "circle":
        k = s(25)
        d.ellipse([c - k, c - k, c + k, c + k], outline=CIRCLE, width=w)
    elif symbol == "square":
        k = s(22)
        d.rectangle([c - k, c - k, c + k, c + k], outline=SQUARE, width=w)
    elif symbol == "triangle":
        k = s(28)
        pts = [(c, c - k), (c + k * math.sin(math.radians(60)), c + k / 2),
               (c - k * math.sin(math.radians(60)), c + k / 2)]
        d.line(pts + [pts[0], pts[1]], fill=TRIANGLE, width=w, joint="curve")
    return done(im)


def label(d, text, cx, cy, size, fill=DARK):
    f = font(size)
    box = d.textbbox((0, 0), text, font=f)
    d.text((cx - (box[0] + box[2]) / 2, cy - (box[1] + box[3]) / 2), text, font=f, fill=fill)


def options():
    im, d = canvas()
    d.rounded_rectangle([s(10), s(26), s(118), s(102)], radius=s(38), fill=RIM)
    d.rounded_rectangle([s(16), s(32), s(112), s(96)], radius=s(32), fill=DARK)
    for y in (48, 64, 80):
        d.rounded_rectangle([s(40), s(y - 4), s(88), s(y + 4)], radius=s(4), fill=WHITE)
    return done(im)


def touchpad():
    im, d = canvas()
    # the pad's shape: wider at the top, as on the DualSense
    outer = [(s(6), s(26)), (s(122), s(26)), (s(112), s(100)), (s(16), s(100))]
    d.polygon(outer, fill=RIM)
    inner = [(s(13), s(32)), (s(115), s(32)), (s(106), s(94)), (s(22), s(94))]
    d.polygon(inner, fill=DARK)
    d.ellipse([s(54), s(53), s(74), s(73)], fill=WHITE)  # (a press)
    return done(im)


def bumper(text, left):
    im, d = canvas()
    box = [s(8), s(30), s(120), s(98)]
    d.rounded_rectangle(box, radius=s(22), fill=RIM)
    # the outer top corner rounder, as the shoulder buttons are
    if left:
        d.pieslice([s(8), s(30), s(72), s(130)], 180, 270, fill=RIM)
    else:
        d.pieslice([s(56), s(30), s(120), s(130)], 270, 360, fill=RIM)
    label(d, text, s(64), s(64), 44)
    return done(im)


def trigger(text, left):
    im, d = canvas()
    if left:
        pts = [(s(30), s(8)), (s(96), s(8)), (s(108), s(30)), (s(108), s(120)), (s(20), s(120)), (s(20), s(30))]
    else:
        pts = [(s(32), s(8)), (s(98), s(8)), (s(108), s(30)), (s(108), s(120)), (s(20), s(120)), (s(20), s(30))]
    d.polygon(pts, fill=RIM)
    d.rounded_rectangle([s(20), s(24), s(108), s(120)], radius=s(16), fill=RIM)
    label(d, text, s(64), s(70), 44)
    return done(im)


# The eight directions, in the font's order: UL L DL D DR R UR U (angles in degrees, 0 = right, y down).
DIRECTIONS = [("ul", 225), ("l", 180), ("dl", 135), ("d", 90), ("dr", 45), ("r", 0), ("ur", 315), ("u", 270)]


def arrow(d, angle, r_tip, size, fill):
    a = math.radians(angle)
    c = s(64)
    tip = (c + math.cos(a) * s(r_tip), c + math.sin(a) * s(r_tip))
    back = r_tip - size
    bx, by = c + math.cos(a) * s(back), c + math.sin(a) * s(back)
    px, py = -math.sin(a), math.cos(a)
    half = s(size) * 0.75
    d.polygon([tip, (bx + px * half, by + py * half), (bx - px * half, by - py * half)], fill=fill)


def stick(letter, angle=None):
    im, d = canvas()
    r = 40 if angle is not None else 54
    c = s(64)
    d.ellipse([c - s(r), c - s(r), c + s(r), c + s(r)], fill=RIM)
    d.ellipse([c - s(r - 5), c - s(r - 5), c + s(r - 5), c + s(r - 5)], fill=DARK)
    label(d, letter, c, c, 34 if angle is not None else 46, fill=WHITE)
    if angle is not None:
        arrow(d, angle, 63, 18, RIM)
    return done(im)


def dpad(lit):
    """The PlayStation D-pad: four arrow-shaped buttons; `lit` the ones pressed ("ul": up and left; all when
    empty)."""
    lit = set(lit)
    im, d = canvas()
    c = s(64)
    for name, angle in (("r", 0), ("d", 90), ("l", 180), ("u", 270)):
        on = not lit or name in lit
        a = math.radians(angle)
        ux, uy = math.cos(a), math.sin(a)
        px, py = -uy, ux

        def p(along, across):
            return (c + ux * s(along) + px * s(across), c + uy * s(along) + py * s(across))

        # a button pointing at the centre: a square end outside, a point inside
        outline = [p(14, 0), p(26, -17), p(60, -17), p(60, 17), p(26, 17)]
        d.polygon(outline, fill=RIM if on else DIM)
        inner = [p(19, 0), p(29, -12), p(55, -12), p(55, 12), p(29, 12)]
        d.polygon(inner, fill=DARK)
        tri = [p(50, 0), p(36, -8), p(36, 8)]  # (pointing out)
        d.polygon(tri, fill=WHITE if on else DIM)
    return done(im)


def sheet():
    cells = [None] * COUNT
    cells[0], cells[1], cells[2], cells[3] = face("cross"), face("circle"), face("square"), face("triangle")
    # 4 LIVE, 5 and 6 unused: the game's own
    cells[7] = options()
    cells[8] = touchpad()
    cells[9], cells[10] = bumper("L1", True), bumper("R1", False)
    cells[11], cells[12] = trigger("L2", True), trigger("R2", False)
    cells[13], cells[14] = stick("L"), stick("R")
    for i, (_, angle) in enumerate(DIRECTIONS):
        cells[15 + i] = stick("L", angle)
        cells[23 + i] = stick("R", angle)
    for i, (name, _) in enumerate(DIRECTIONS):
        cells[31 + i] = dpad(name)
    cells[39] = dpad("")
    rows = (COUNT + COLUMNS - 1) // COLUMNS
    out = Image.new("RGBA", (COLUMNS * CELL, rows * CELL), (0, 0, 0, 0))
    for n, cell in enumerate(cells):
        if cell is not None:
            out.alpha_composite(cell, ((n % COLUMNS) * CELL, (n // COLUMNS) * CELL))
    return out


def write_dds(im, path):
    w, h = im.size
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x8  # caps, height, width, pixelformat, pitch
    header = struct.pack("<4sIIIIIII44x", b"DDS ", 124, flags, h, w, w * 4, 0, 1)
    # DDPIXELFORMAT: RGB with alpha, 32 bits, masks R G B A
    pf = struct.pack("<II4sIIIII", 32, 0x41, b"\0\0\0\0", 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)
    caps = struct.pack("<IIII4x", 0x1000, 0, 0, 0)
    r, g, b, a = im.split()
    pixels = Image.merge("RGBA", (b, g, r, a)).tobytes()  # B G R A in memory = A8R8G8B8
    with open(path, "wb") as f:
        f.write(header + pf + caps + pixels)


def main():
    os.makedirs(OUT, exist_ok=True)
    im = sheet()
    write_dds(im, os.path.join(OUT, "ps_button_glyphs.dds"))
    for name, symbol in (("ps_cross_40.png", "cross"), ("ps_circle_40.png", "circle")):
        face(symbol).resize((40, 40), Image.LANCZOS).save(os.path.join(OUT, name), optimize=True)
    preview = Image.new("RGBA", im.size, (110, 110, 120, 255))
    preview.alpha_composite(im)
    preview.convert("RGB").save(os.path.join(OUT, "preview.png"))
    print("wrote", OUT, im.size)


if __name__ == "__main__":
    main()
