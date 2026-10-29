#!/usr/bin/env python3
"""The home screen art for the app's sce_sys: icon0.png (512x512) and
pic0.dds / pic1.dds (3840x2160 BC7, as the PS5 takes them).

    make_sce_art.py <sce_sys dir> [--icon CrackdownLogo.png]
                    [--background CrackdownBackground.jpg] [--title-font bahnschrift.ttf]

With the user's own art (kept out of git) the tile is the white logo with the
background's neon glow over a darkened piece of the city, and the background
is that picture at 4K (BC7 mode 6, needs numpy); "REACTIVATED" goes under the
logo on both, in the logo's neon look (Bahnschrift Bold is the closest to its
squared lettering). Without them (the CI build) a title card is drawn with
Pillow and stored with one colour per 4x4 block.
"""
import argparse

from PIL import Image, ImageDraw, ImageFont

TITLE = "CRACKDOWN"
SUB = "ReActivated"
BG_TOP = (8, 14, 20)
BG_BOTTOM = (10, 52, 40)
FG = (235, 225, 210)
ACCENT = (120, 220, 40)

# DDS header (DX10, BC7_UNORM 98, 3840x2160, one mip) as the PS5 apps ship it.
DDS_HEADER = bytes.fromhex(
    "444453207c0000000710080070080000000f000000907e00000000000100000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000002000000004000000445831300000000000000000000000000000000000000000"
    "001000000000000000000000000000000000000062000000030000000000000001000000"
    "00000000")


def font(size):
    for name in ("DejaVuSans-Bold.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            pass
    return ImageFont.load_default(size=size)


def picture(w, h, title_size, sub_size):
    img = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(img)
    for y in range(h):
        t = y / max(1, h - 1)
        d.line([(0, y), (w, y)], fill=tuple(int(a + (b - a) * t) for a, b in zip(BG_TOP, BG_BOTTOM)))
    tf, sf = font(title_size), font(sub_size)
    tw = d.textlength(TITLE, font=tf)
    sw = d.textlength(SUB, font=sf)
    ty = h * 0.40
    d.text(((w - tw) / 2, ty), TITLE, font=tf, fill=FG)
    d.rectangle([(w - tw) / 2, ty + title_size * 1.15, (w + tw) / 2, ty + title_size * 1.15 + max(2, h // 120)],
                fill=ACCENT)
    d.text(((w - sw) / 2, ty + title_size * 1.4), SUB, font=sf, fill=FG)
    return img


def bc7_mode6_solid(r, g, b, a=255):
    """One BC7 block, every texel (r, g, b, a): mode 6, both endpoints equal."""
    p = (r + g + b) // 3 & 1  # one shared low bit per endpoint
    c = [r >> 1, g >> 1, b >> 1, a >> 1]
    bits, n = 1 << 6, 7  # mode 6: bit 6 set
    for ch in c:
        for _ in range(2):  # endpoint 0 and 1
            bits |= ch << n
            n += 7
    bits |= p << n
    bits |= p << (n + 1)
    return bits.to_bytes(16, "little")  # indices all 0


def write_dds(path, img):
    small = img.resize((3840 // 4, 2160 // 4), Image.LANCZOS)
    px = small.load()
    out = bytearray(DDS_HEADER)
    cache = {}
    for y in range(small.height):
        for x in range(small.width):
            c = px[x, y]
            blk = cache.get(c)
            if blk is None:
                blk = cache[c] = bc7_mode6_solid(*c)
            out += blk
    with open(path, "wb") as f:
        f.write(out)


W, H = 3840, 2160
NAME = "REACTIVATED"
NEON = (40, 150, 255)  # the logo's glow



def neon_title(font_path, cap_px, glow=1.0, bars=True):
    """NAME in the logo's look, as an RGBA picture: white letters (Bahnschrift
    Bold, the closest to the logo's squared lettering, spaced out), a blue
    neon glow round them, and a bar either side like the logo's wings.
    `cap_px`: the capitals' height."""
    from PIL import ImageChops, ImageFilter

    probe = ImageFont.truetype(font_path, 200)
    try:
        probe.set_variation_by_name("Bold")
    except (OSError, ValueError):
        pass
    cap = probe.getbbox("E")[3] - probe.getbbox("E")[1]
    size = max(8, round(200 * cap_px / cap))
    fnt = ImageFont.truetype(font_path, size)
    try:
        fnt.set_variation_by_name("Bold")
    except (OSError, ValueError):
        pass
    spacing = round(cap_px * 0.16)
    widths = [fnt.getlength(c) for c in NAME]
    text_w = round(sum(widths) + spacing * (len(NAME) - 1))
    bar_w = round(cap_px * 2.6) if bars else 0
    gap = round(cap_px * 0.6) if bars else 0
    pad = round(cap_px * 1.4)
    w = text_w + 2 * (bar_w + gap) + 2 * pad
    h = round(cap_px * 1.6) + 2 * pad
    mask = Image.new("L", (w, h))
    d = ImageDraw.Draw(mask)
    top = pad + round(cap_px * 0.3) - (fnt.getbbox("E")[1])
    x = pad + bar_w + gap
    for c, cw in zip(NAME, widths):
        d.text((x, top), c, font=fnt, fill=255)
        x += cw + spacing
    if bars:
        # the wings: thin bars, cut at a slant at their outer ends
        y0 = pad + round(cap_px * 0.3) + round(cap_px * 0.42)
        t = max(2, round(cap_px * 0.16))
        for side in (0, 1):
            x0 = pad if side == 0 else w - pad - bar_w
            x1 = x0 + bar_w
            slant = round(cap_px * 0.5)
            if side == 0:
                poly = [(x0 + slant, y0), (x1, y0), (x1, y0 + t), (x0, y0 + t)]
            else:
                poly = [(x0, y0), (x1 - slant, y0), (x1, y0 + t), (x0, y0 + t)]
            d.polygon(poly, fill=255)
    out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    for radius, strength in ((cap_px * 0.9, 0.55), (cap_px * 0.32, 0.85), (cap_px * 0.1, 1.0)):
        g = mask.filter(ImageFilter.GaussianBlur(max(1, radius)))
        g = g.point(lambda v, s=strength * glow: min(255, int(v * s * 1.6)))
        layer = Image.new("RGBA", (w, h), NEON + (0,))
        layer.putalpha(g)
        out = Image.alpha_composite(out, layer)
    core = Image.new("RGBA", (w, h), (232, 246, 255, 0))
    core.putalpha(mask)
    return Image.alpha_composite(out, core)


def put_centered(img, layer, cx, cy):
    img = img.convert("RGBA")
    img.alpha_composite(layer, (round(cx - layer.width / 2), round(cy - layer.height / 2)))
    return img.convert("RGB")


def cover(img, w, h):
    """`img` scaled to cover w x h and cut to it (centred)."""
    img = img.convert("RGB")
    s = max(w / img.width, h / img.height)
    big = img.resize((max(w, round(img.width * s)), max(h, round(img.height * s))), Image.LANCZOS)
    x, y = (big.width - w) // 2, (big.height - h) // 2
    return big.crop((x, y, x + w, y + h))


def background_art(path, font_path):
    """The background (CrackdownBackground.jpg, the neon logo over Pacific City)
    at 4K, NAME under the logo's lower point."""
    bg = cover(Image.open(path), W, H)
    if font_path:
        title = neon_title(font_path, round(H * 0.040))
        bg = put_centered(bg, title, W / 2, H * 0.835)
    return bg


def logo_shape(logo_path):
    """The logo's shape as a mask, cut to it (white on black or on transparent:
    its brightness is its shape)."""
    logo = Image.open(logo_path).convert("RGBA")
    shape = Image.composite(logo.convert("L"), Image.new("L", logo.size), logo.getchannel("A"))
    return shape.crop(shape.point(lambda v: 255 if v > 24 else 0).getbbox())


def menu_logo(logo_path, font_path, width=1200):
    """The menu's HOME logo (hub/logo.png): the white logo with the neon glow
    and NAME under it, on transparent."""
    from PIL import ImageFilter

    shape = logo_shape(logo_path)
    shape = shape.resize((width, round(shape.height * width / shape.width)), Image.LANCZOS)
    title = neon_title(font_path, round(width * 0.052)) if font_path else None
    pad = round(width * 0.06)
    th = title.height if title else 0
    out = Image.new("RGBA", (width + 2 * pad, shape.height + 2 * pad + th), NEON + (0,))
    glow = Image.new("L", out.size)
    glow.paste(shape, (pad, pad))
    out.putalpha(glow.filter(ImageFilter.GaussianBlur(width * 0.012)).point(lambda v: min(255, int(v * 1.4))))
    white = Image.new("RGBA", out.size, (240, 248, 255, 0))
    core = Image.new("L", out.size)
    core.paste(shape, (pad, pad))
    white.putalpha(core)
    out = Image.alpha_composite(out, white)
    if title:
        out.alpha_composite(title, (round((out.width - title.width) / 2), pad + shape.height - round(th * 0.2)))
    return out.crop(out.getchannel("A").point(lambda v: 255 if v > 6 else 0).getbbox())


def icon_art(logo_path, background_path, font_path):
    """The tile: the white logo (CrackdownLogo.png) with the neon glow, over a
    darkened piece of the city, NAME under it."""
    from PIL import ImageFilter

    s = 512 * 2  # drawn at twice the size, then scaled down
    if background_path:
        bg = Image.open(background_path).convert("RGB")
        # the city to the right of the logo, dark and soft
        city = bg.crop((round(bg.width * 0.75), round(bg.height * 0.13), round(bg.width * 0.995),
                        round(bg.height * 0.87)))
        base = cover(city, s, s).filter(ImageFilter.GaussianBlur(6))
        base = Image.blend(base, Image.new("RGB", base.size, (2, 6, 14)), 0.55)
    else:
        base = Image.new("RGB", (s, s), (4, 10, 22))
    shape = logo_shape(logo_path)
    scale = min(s * 0.84 / shape.width, s * 0.66 / shape.height)
    shape = shape.resize((round(shape.width * scale), round(shape.height * scale)), Image.LANCZOS)
    logo = Image.new("RGBA", shape.size, (255, 255, 255, 0))
    logo.putalpha(shape)
    alpha = logo.getchannel("A")
    glow = Image.new("RGBA", logo.size, NEON + (0,))
    glow.putalpha(alpha.filter(ImageFilter.GaussianBlur(s * 0.012)).point(lambda v: min(255, int(v * 1.5))))
    lx, ly = (s - logo.width) // 2, round(s * 0.08)
    img = base.convert("RGBA")
    pad = round(s * 0.03)
    glow_big = Image.new("RGBA", (logo.width + 2 * pad, logo.height + 2 * pad), NEON + (0,))
    glow_big.paste(glow, (pad, pad))
    glow_big = glow_big.filter(ImageFilter.GaussianBlur(s * 0.01))
    img.alpha_composite(glow_big, (lx - pad, ly - pad))
    img.alpha_composite(logo, (lx, ly))
    img = img.convert("RGB")
    if font_path:
        title = neon_title(font_path, round(s * 0.058), bars=True)
        img = put_centered(img, title, s / 2, ly + logo.height + s * 0.085)
    return img.resize((512, 512), Image.LANCZOS)


def bc7_mode6(img):
    """BC7 mode 6, opaque: per block, the colours' range along their main axis
    as the two endpoints (odd 8-bit values: p-bit 1, so alpha is 255) and each
    texel's nearest of the 16 levels between them."""
    import numpy as np

    a = np.asarray(img.convert("RGB"), dtype=np.float32)
    bh, bw = H // 4, W // 4
    blocks = a.reshape(bh, 4, bw, 4, 3).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 3)
    mean = blocks.mean(axis=1, keepdims=True)
    cen = blocks - mean
    # the main axis: a few power iterations on the covariance
    cov = np.einsum("nki,nkj->nij", cen, cen)
    axis = np.ones((len(blocks), 3), np.float32)
    for _ in range(6):
        axis = np.einsum("nij,nj->ni", cov, axis)
        axis /= np.maximum(np.linalg.norm(axis, axis=1, keepdims=True), 1e-6)
    t = np.einsum("nki,ni->nk", cen, axis)
    e0 = mean[:, 0] + axis * t.min(axis=1, keepdims=True)
    e1 = mean[:, 0] + axis * t.max(axis=1, keepdims=True)
    # endpoints as 7 bits + p-bit 1: 8-bit value 2v+1
    q0 = np.clip(np.round((e0 - 1) / 2), 0, 127).astype(np.int64)
    q1 = np.clip(np.round((e1 - 1) / 2), 0, 127).astype(np.int64)
    c0, c1 = (q0 * 2 + 1)[:, None, :], (q1 * 2 + 1)[:, None, :]
    weights = np.array([0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64], np.int64)
    pal = ((64 - weights)[None, :, None] * c0 + weights[None, :, None] * c1 + 32) >> 6  # n,16,3
    idx = np.empty((len(blocks), 16), np.int64)
    for s in range(0, len(blocks), 65536):
        d = ((blocks[s:s + 65536, :, None, :] - pal[s:s + 65536, None, :, :].astype(np.float32)) ** 2).sum(-1)
        idx[s:s + 65536] = d.argmin(-1)
    # the first texel's index must have its top bit clear: swap the endpoints where it hasn't
    swap = idx[:, 0] >= 8
    q0[swap], q1[swap] = q1[swap].copy(), q0[swap].copy()
    idx[swap] = 15 - idx[swap]

    out = np.zeros((len(blocks), 2), np.uint64)  # low and high 64 bits
    def put(value, pos, nbits):
        value = value.astype(np.uint64)
        if pos + nbits <= 64:
            out[:, 0] |= value << np.uint64(pos)
        elif pos >= 64:
            out[:, 1] |= value << np.uint64(pos - 64)
        else:
            k = 64 - pos
            out[:, 0] |= (value & np.uint64((1 << k) - 1)) << np.uint64(pos)
            out[:, 1] |= value >> np.uint64(k)
        return pos + nbits
    pos = put(np.full(len(blocks), 1 << 6), 0, 7)
    for ch in range(3):
        pos = put(q0[:, ch], pos, 7)
        pos = put(q1[:, ch], pos, 7)
    pos = put(np.full(len(blocks), 127), pos, 7)  # alpha
    pos = put(np.full(len(blocks), 127), pos, 7)
    pos = put(np.ones(len(blocks)), pos, 1)  # p-bits
    pos = put(np.ones(len(blocks)), pos, 1)
    pos = put(idx[:, 0], pos, 3)
    for i in range(1, 16):
        pos = put(idx[:, i], pos, 4)
    assert pos == 128
    return out.astype("<u8").tobytes()



def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--icon", help="the logo (CrackdownLogo.png, white on transparent)")
    ap.add_argument("--background", help="CrackdownBackground.jpg")
    ap.add_argument("--title-font", help="Bahnschrift (bahnschrift.ttf)")
    ap.add_argument("--preview", help="also save the background as this PNG")
    ap.add_argument("--menu-logo", help="also save the menu's wordmark (hub/logo.png) here: NAME in neon "
                    "(the menu's background has the logo itself)")
    args = ap.parse_args()
    if args.menu_logo and args.title_font:
        mark = neon_title(args.title_font, 96)
        mark.crop(mark.getchannel("A").point(lambda v: 255 if v > 6 else 0).getbbox()).save(args.menu_logo,
                                                                                             optimize=True)
    if args.icon:
        icon_art(args.icon, args.background, args.title_font).save(f"{args.out}/icon0.png", optimize=True)
    else:
        picture(512, 512, 78, 26).save(f"{args.out}/icon0.png")
    if args.background:
        bg = background_art(args.background, args.title_font)
        if args.preview:
            bg.resize((1920, 1080), Image.LANCZOS).save(args.preview)
        data = DDS_HEADER + bc7_mode6(bg)
        for name in ("pic0.dds", "pic1.dds"):
            with open(f"{args.out}/{name}", "wb") as f:
                f.write(data)
    else:
        big = picture(1920, 1080, 190, 52)
        write_dds(f"{args.out}/pic0.dds", big)
        write_dds(f"{args.out}/pic1.dds", big)
    print("icon0.png, pic0.dds, pic1.dds")


if __name__ == "__main__":
    main()
