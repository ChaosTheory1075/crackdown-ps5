"""Crackdown: ReActivated's header title (hub/title.png): the game's white CRACKDOWN wordmark (cut out of
the store logo) and REACTIVATED in the logo's neon, on one line.
    cd_title.py <crackdown-ps5 repo> <logo-transparent.png> <bahnschrift.ttf> <out.png> [height]"""
import sys
import numpy as np
from PIL import Image, ImageFilter
repo, logo_path, font_path, out_path = sys.argv[1:5]
height = int(sys.argv[5]) if len(sys.argv) > 5 else 96
sys.path.insert(0, repo + "/ci")
import make_sce_art as m  # noqa: E402
im = np.asarray(Image.open(logo_path).convert("RGBA"), np.float32)
band = im[297:381, 200:1100]  # (the letters' rows: the emblem's wings are above and below)
white = np.clip((band[..., :3].min(axis=2) - 150) / 70, 0, 1) * (band[..., 3] / 255)
mask = Image.fromarray((white * 255).astype(np.uint8))
mask = mask.crop(mask.getbbox())
cap = mask.height
def neon(shape, pad, radii):
    w, h = shape.width + 2 * pad, shape.height + 2 * pad
    full = Image.new("L", (w, h)); full.paste(shape, (pad, pad))
    out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    for radius, strength in radii:
        g = full.filter(ImageFilter.GaussianBlur(max(1, radius))).point(lambda v, s=strength: min(255, int(v * s * 1.6)))
        layer = Image.new("RGBA", (w, h), m.NEON + (0,)); layer.putalpha(g)
        out = Image.alpha_composite(out, layer)
    core = Image.new("RGBA", (w, h), (240, 248, 255, 0)); core.putalpha(full)
    return Image.alpha_composite(out, core)
radii = lambda c: ((c * 0.28, 0.55), (c * 0.1, 0.9), (c * 0.035, 1.0))
pad = round(cap * 0.5)
word = neon(mask, pad, radii(cap))
# REACTIVATED: Bahnschrift Bold, spaced, its capitals 0.55 of the wordmark's, on its baseline
from PIL import ImageDraw, ImageFont
scap = round(cap * 0.55)
probe = ImageFont.truetype(font_path, 200)
try: probe.set_variation_by_name("Bold")
except (OSError, ValueError): pass
e = probe.getbbox("E"); fnt = ImageFont.truetype(font_path, round(200 * scap / (e[3] - e[1])))
try: fnt.set_variation_by_name("Bold")
except (OSError, ValueError): pass
sp = round(scap * 0.2)
ws = [fnt.getlength(c) for c in m.NAME]
sm = Image.new("L", (round(sum(ws) + sp * (len(ws) - 1)) + 4, scap + 4))
x = 0
for c, cw in zip(m.NAME, ws):
    ImageDraw.Draw(sm).text((x, -fnt.getbbox("E")[1]), c, font=fnt, fill=255); x += cw + sp
sm = sm.crop(sm.getbbox())
sub = neon(sm, pad, radii(scap))
gap = round(cap * 0.45)
W = word.width + gap + sub.width - pad
out = Image.new("RGBA", (W, word.height), (0, 0, 0, 0))
out.alpha_composite(word, (0, 0))
out.alpha_composite(sub, (word.width - pad + gap - pad, pad + cap - sm.height - pad))
out = out.crop(out.getchannel("A").point(lambda v: 255 if v > 40 else 0).getbbox())
# (the letters at 0.8 of the header's height: the wide wordmark as wide as the other ports' titles)
inner = round(height * 0.8)
small = out.resize((round(out.width * inner / out.height), inner), Image.LANCZOS)
final = Image.new("RGBA", (small.width, height), (0, 0, 0, 0))
final.alpha_composite(small, (0, (height - inner) // 2))
final.save(out_path)
print(out_path, out.size)
