#!/usr/bin/env python3
"""Labelled contact sheet of every PNG in an extracted archive folder.

usage: contact_sheet.py <archive dir> <out.png> [--skin skin.json] [--title TEXT]
                        [--exclude REGEX] [--width 1600]

Each tile shows the image on a checkerboard (alpha visible), scaled down to fit,
with file name, pixel size, and (when --skin is given) the skin ids/styles that
reference it.  Sprite strips used as button states are annotated with the RECT
slices from the skin (thin coloured lines).
"""
import json, os, re, sys
from PIL import Image, ImageDraw, ImageFont

FONT_PATHS = ['/System/Library/Fonts/Hiragino Sans GB.ttc', '/System/Library/Fonts/STHeiti Medium.ttc',
              '/System/Library/Fonts/Supplemental/Arial Unicode.ttf']


def font(size):
    for p in FONT_PATHS:
        if os.path.exists(p):
            try: return ImageFont.truetype(p, size)
            except Exception: pass
    return ImageFont.load_default()


def checker(w, h, s=8):
    im = Image.new('RGB', (w, h), (200, 200, 200))
    d = ImageDraw.Draw(im)
    for y in range(0, h, s):
        for x in range((y // s) % 2 * s, w, 2 * s):
            d.rectangle([x, y, x + s - 1, y + s - 1], fill=(235, 235, 235))
    return im


def main():
    a = sys.argv[1:]
    src, out = a[0], a[1]
    opt = lambda k, dflt=None: a[a.index(k) + 1] if k in a else dflt
    sheet_w = int(opt('--width', 1600))
    title = opt('--title', src)
    excl = re.compile(opt('--exclude')) if opt('--exclude') else None
    usage, slices = {}, {}
    if opt('--skin'):
        sk = json.load(open(opt('--skin'), encoding='utf-8'))
        for rid, r in sk['resources'].items():
            key = (r['src'] or '').rsplit('.', 1)[0].lower()
            usage.setdefault(key, []).extend([rid] + r['used_by'])
        for sid, st in sk['styles'].items():
            for s, v in st.get('states', {}).items():
                for k in ('image', 'bar'):
                    r = v.get(k)
                    if r and r.get('file') and r.get('rect'):
                        slices.setdefault(r['file'].rsplit('.', 1)[0].lower(), set()).add(tuple(r['rect']))
    files = sorted(f for f in os.listdir(src) if f.endswith('.png') and not (excl and excl.search(f)))
    f_lbl, f_small, f_title = font(15), font(12), font(26)
    pad, label_h = 12, 54
    max_tile_w = (sheet_w - pad * 4) // 3
    tiles = []
    for fn in files:
        im = Image.open(os.path.join(src, fn)).convert('RGBA')
        w, h = im.size
        # big images: half of the sheet width; small: as-is (upscale tiny icons x2)
        limit = max_tile_w * 2 + pad if w >= 600 else max_tile_w
        scale = min(1.0, limit / w, 360 / h) if w > 0 else 1
        if w <= 64 and h <= 64: scale = 2.0
        tw, th = max(1, int(w * scale)), max(1, int(h * scale))
        bg = checker(tw, th)
        bg.paste(im.resize((tw, th), Image.LANCZOS if scale < 1 else Image.NEAREST), (0, 0), im.resize((tw, th)))
        dr = ImageDraw.Draw(bg)
        stem = fn[:-4].lower()
        for (x, y, rw, rh) in sorted(slices.get(stem, [])):
            if (rw, rh) == (w, h): continue
            dr.rectangle([int(x * scale), int(y * scale), int((x + rw) * scale) - 1, int((y + rh) * scale) - 1],
                         outline=(255, 0, 160))
        u = [x for x in usage.get(stem, []) if not x.startswith('ID_FRM') and not x.startswith('ID_')] \
            or usage.get(stem, [])[:1]
        u2 = sorted(set(x.split('.')[0] for x in usage.get(stem, [])[1:]))
        tiles.append((fn, (w, h), scale, bg, u2))
    # layout rows
    x = y = pad
    y += 40
    row_h = 0
    placed = []
    for t in tiles:
        tw = max(t[3].size[0], 220)
        th = t[3].size[1] + label_h
        if x + tw > sheet_w - pad:
            x = pad; y += row_h + pad; row_h = 0
        placed.append((t, x, y))
        x += tw + pad
        row_h = max(row_h, th)
    H = y + row_h + pad
    sheet = Image.new('RGB', (sheet_w, H), (250, 250, 248))
    d = ImageDraw.Draw(sheet)
    d.text((pad, 8), title, fill=(20, 20, 20), font=f_title)
    for (fn, (w, h), scale, img, u2), x, y in placed:
        sheet.paste(img, (x, y))
        d.rectangle([x - 1, y - 1, x + img.size[0], y + img.size[1]], outline=(150, 150, 150))
        ly = y + img.size[1] + 3
        d.text((x, ly), '%s  %dx%d%s' % (fn[:-4], w, h, '' if scale == 1 else '  (x%.2g)' % scale), fill=(0, 0, 0), font=f_lbl)
        if u2:
            txt = ', '.join(s.replace('ID_', '') for s in u2)
            maxc = max(30, int(max(img.size[0], 220) / 6.5))
            lines = [txt[i:i + maxc] for i in range(0, min(len(txt), maxc * 2), maxc)]
            for i, l in enumerate(lines):
                d.text((x, ly + 19 + 14 * i), l, fill=(90, 60, 130), font=f_small)
    sheet.save(out, optimize=True)
    print(out, sheet.size, len(tiles), 'images')


if __name__ == '__main__':
    main()
