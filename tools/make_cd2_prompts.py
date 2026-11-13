#!/usr/bin/env python3
"""Crackdown 2's PlayStation button prompts: the art and the rules cd2/src/cd2_prompts.cpp applies at start-up.

    make_cd2_prompts.py <extracted UI folder> [out dir]      (default out: package/glyphs)

Crackdown 2 draws its buttons as vector shapes inside its Scaleform movies (UI\\ui.pack, pause.pack,
initial.pack): the A/B/X/Y discs, Back and Start, the bumpers and triggers, the sticks and the D-pad
(HUD\\Prompt.gfx, HUD\\controls.gfx, UI\\FePrompt.gfx, UI\\Framework.gfx, UI\\ShortcutPanel.gfx ...). The same art
is copied into each movie that uses it, byte for byte, so each one is found by a hash of its DefineShape tag
(FNV-1a 64 of the tag code byte and the tag's bytes after the shape id) and replaced by a shape of the same id
and bounds filled with a bitmap of the PlayStation one: the glyphs of tools/make_ps_button_glyphs.py, as
Crackdown 1 shows them.

The extracted folder (pack.py / ex.py of Downloads/Crackdown/Extras/cd2tools on the user's own game files) is
only read here, for the hashes and to check that every rule still finds its shape; cd2_prompts.bin carries
nothing of the game's: the hashes and our own pictures.

Out: cd2_prompts.bin; cd2_prompts_preview.png (each original beside its replacement, needs swfshape.py).

cd2_prompts.bin (little endian): "CD2P", u32 version 1, u32 rules; per rule: u64 hash, u16 width, u16 height,
u32 bytes, then that many bytes: zlib of the picture as DefineBitsLossless2 format 5 wants it (rows of
premultiplied A, R, G, B).
"""
import glob
import hashlib
import os
import struct
import sys
import zlib

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import make_ps_button_glyphs as g  # noqa: E402

TOOLS = os.path.join(os.path.dirname(HERE), "..", "..", "Crackdown", "Extras", "cd2tools")

# (md5 prefix of the shape tag as cd2tools/uniq.py lists it, what replaces it, a note)
RULES = [
    ("7b6c9c2dc2", "cross", "A (Prompt, controls)"),
    ("6743bd3466", "cross", "A (FePrompt, Framework, Audio)"),
    ("6fa6b12fa9", "circle", "B (FePrompt, Prompt, controls)"),
    ("ec9fda669c", "circle", "B (Framework, FeWaiting)"),
    ("d87d3fc099", "square", "X"),
    ("3ba1f25b6f", "triangle", "Y (FePrompt, Prompt, controls)"),
    ("ac5e1adf0b", "triangle", "Y (Framework)"),
    ("9b6b74e714", "stick_r", "RS"),
    ("c90a61259c", "stick_l", "LS"),
    ("de0f6b95bb", "dpad", "D-pad (down lit)"),
    ("d8ee0fdbbd", "touchpad", "Back badge (Prompt)"),
    ("e54b108cf2", "trigger_r", "RT (controls)"),
    ("d0b59e6b9a", "trigger_l", "LT (controls)"),
    # the orange bases under a text overlay, and the overlays
    ("9d7e7d29e7", "base_bumper", "bumper/Start/Back base (controls 17)"),
    ("27e211a98d", "base_bumper", "bumper base (ShortcutPanel 24)"),
    ("d0f1ca1b44", "base_trigger", "trigger base (ShortcutPanel 16)"),
    ("8c24a0903a", "ov_options", "Start arrow (controls 32)"),
    ("a70978dfb1", "ov_touchpad", "Back arrow (controls 34)"),
    ("57db91813d", "ov_L1", "LB (controls 19)"),
    ("a7ad962cc7", "ov_L1", "LB (ShortcutPanel 32)"),
    ("aabd4a47d2", "ov_R1", "RB (controls 28)"),
    ("eaefced16b", "ov_R1", "RB (ShortcutPanel 26)"),
    ("518efbb8d1", "ov_R2", "RT (ShortcutPanel 18)"),
]

SS = 4


def fit(art, w, h, px=160):
    """`art` (square) centred in a picture of the shape's aspect, its height `px`."""
    H = px
    W = max(8, round(px * w / h))
    out = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    side = min(W, H)
    a = art.resize((side, side), Image.LANCZOS)
    out.alpha_composite(a, ((W - side) // 2, (H - side) // 2))
    return out


def draw_box(w, h, painter, px=160):
    W = max(8, round(px * w / h))
    im = Image.new("RGBA", (W * SS, px * SS), (0, 0, 0, 0))
    painter(ImageDraw.Draw(im), W * SS, px * SS)
    return im.resize((W, px), Image.LANCZOS)


def text_fit(d, text, W, H, fill, frac=0.78):
    size = int(H * frac)
    while size > 8:
        f = g.ImageFont.truetype(g.FONT, size)
        b = d.textbbox((0, 0), text, font=f)
        if b[2] - b[0] <= W * 0.92 and b[3] - b[1] <= H * 0.86:
            break
        size -= 2
    d.text(((W - (b[0] + b[2])) / 2, (H - (b[1] + b[3])) / 2), text, font=f, fill=fill)


def art(kind, w, h):
    if kind in ("cross", "circle", "square", "triangle"):
        return fit(g.face(kind), w, h)
    if kind == "stick_l":
        return fit(g.stick("L"), w, h)
    if kind == "stick_r":
        return fit(g.stick("R"), w, h)
    if kind == "dpad":
        return fit(g.dpad("d"), w, h)
    if kind == "touchpad":
        return fit(g.touchpad(), w, h)
    if kind in ("trigger_l", "trigger_r"):
        # (the game's are wide badges: the same, light, with the PlayStation name)
        def paint(d, W, H, text="L2" if kind == "trigger_l" else "R2"):
            d.rounded_rectangle([0, 0, W - 1, H - 1], radius=H * 0.30, fill=g.RIM)
            box = Image.new("RGBA", (W, H))
            text_fit(d, text, W, H, g.DARK, frac=0.62)
        return draw_box(w, h, paint)
    if kind == "base_bumper":
        return draw_box(w, h, lambda d, W, H: d.rounded_rectangle([0, 0, W - 1, H - 1], radius=H * 0.36, fill=g.RIM))
    if kind == "base_trigger":
        def paint(d, W, H):
            d.rounded_rectangle([0, 0, W - 1, H - 1], radius=H * 0.30, fill=g.RIM)
        return draw_box(w, h, paint)
    if kind.startswith("ov_") and kind[3:] in ("L1", "R1", "L2", "R2"):
        return draw_box(w, h, lambda d, W, H: text_fit(d, kind[3:], W, H, g.DARK))
    if kind == "ov_options":
        def paint(d, W, H):
            for i in range(3):
                y = H * (0.22 + 0.28 * i)
                d.rounded_rectangle([W * 0.18, y, W * 0.82, y + H * 0.14], radius=H * 0.07, fill=g.DARK)
        return draw_box(w, h, paint)
    if kind == "ov_touchpad":
        def paint(d, W, H):
            t = H * 0.12
            outer = [(W * 0.08, H * 0.12), (W * 0.92, H * 0.12), (W * 0.84, H * 0.88), (W * 0.16, H * 0.88)]
            d.polygon(outer, fill=g.DARK)
            inner = [(W * 0.08 + t, H * 0.12 + t), (W * 0.92 - t, H * 0.12 + t), (W * 0.84 - t * 0.9, H * 0.88 - t),
                     (W * 0.16 + t * 0.9, H * 0.88 - t)]
            d.polygon(inner, fill=(0, 0, 0, 0))
        return draw_box(w, h, paint)
    raise ValueError(kind)


def fnv1a64(data):
    hsh = 0xCBF29CE484222325
    for b in data:
        hsh ^= b
        hsh = (hsh * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return hsh


def argb_premultiplied(im):
    px = im.convert("RGBA").tobytes()
    out = bytearray(len(px))
    for i in range(0, len(px), 4):
        r, gg, b, a = px[i:i + 4]
        out[i] = a
        out[i + 1] = r * a // 255
        out[i + 2] = gg * a // 255
        out[i + 3] = b * a // 255
    return bytes(out)


def main():
    ui = sys.argv[1]
    out_dir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, "..", "package", "glyphs")
    sys.path.insert(0, TOOLS)
    import swfshape as S
    found = {}
    for f in sorted(set(glob.glob(os.path.join(ui, "**", "*.gfx"), recursive=True))):
        body, tags, shapes, _, _ = S.shapes_and_sprites(open(f, "rb").read())
        for sid, s in shapes.items():
            if s[3] is None:
                continue
            code, p, ln = s[0], s[1], s[2]
            key = hashlib.md5(bytes([code]) + body[p + 2:p + ln]).hexdigest()[:10]
            if key in found:
                found[key][3].append(os.path.relpath(f, ui))
                continue
            found[key] = (fnv1a64(bytes([code]) + body[p + 2:p + ln]), s, sid, [os.path.relpath(f, ui)])
    blob = bytearray(b"CD2P" + struct.pack("<II", 1, len(RULES)))
    previews = []
    for key, kind, note in RULES:
        if key not in found:
            sys.exit(f"make_cd2_prompts: no shape {key} ({note}) in {ui}")
        h64, s, sid, movies = found[key]
        x0, x1, y0, y1 = s[3]
        im = art(kind, x1 - x0, y1 - y0)
        data = zlib.compress(argb_premultiplied(im), 9)
        blob += struct.pack("<QHHI", h64, im.width, im.height, len(data)) + data
        previews.append((S.render(s, 96), im))
        print(f"{key} {h64:016X} {kind:12} {im.width}x{im.height} {len(data):6} B  {note}  ({len(movies)} movies)")
    os.makedirs(out_dir, exist_ok=True)
    open(os.path.join(out_dir, "cd2_prompts.bin"), "wb").write(blob)
    cw = 112
    sheet = Image.new("RGBA", (cw * 2 * 4, cw * ((len(previews) + 3) // 4)), (120, 120, 120, 255))
    for i, (a, b) in enumerate(previews):
        x, y = (i % 4) * cw * 2, (i // 4) * cw
        a.thumbnail((cw - 8, cw - 8))
        b.thumbnail((cw - 8, cw - 8))
        sheet.alpha_composite(a, (x + 4, y + 4))
        sheet.alpha_composite(b, (x + cw + 4, y + 4))
    sheet.save(os.path.join(out_dir, "cd2_prompts_preview.png"))
    print(f"{len(RULES)} rules, cd2_prompts.bin {len(blob)} bytes")


if __name__ == "__main__":
    main()
