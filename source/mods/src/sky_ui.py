"""『空澄』(Sky Glass) — the redesigned in-game interface for both games.

A bright, modern visual-novel interface in the games' own summer-sky colours:
the original teal-to-sky dialogue window refined into lit glass, frosted
white system pages with deep-teal ink, a quiet menu bar with small icons,
floating choice pills, thumbnail-first save cards and a grouped settings form. Every texture is drawn at 4x (the master, which doubles
as the HD companion) and reduced to the logical size the script expects.

Text that the script draws (dialogue, names, save numbers, dates, comments,
history, confirm questions, choices) is never baked in; the skin XML styles
for that text are adjusted instead (see FRAME_XML_EDITS). UI labels that the
original artwork bakes in are redrawn here with system fonts at build time.
"""
from __future__ import annotations

from functools import lru_cache
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

S = 4
ROOT = Path(__file__).resolve().parents[2]

CJK = "/System/Library/Fonts/Hiragino Sans GB.ttc"
CJK_W3, CJK_W6 = (CJK, 0), (CJK, 2)
AVENIR = "/System/Library/Fonts/Avenir Next.ttc"
LATIN_DEMI, LATIN_MEDIUM, LATIN_REGULAR = (AVENIR, 2), (AVENIR, 5), (AVENIR, 7)
LATIN_ULTRA = (AVENIR, 10)

# The games' own interface letters are rounded; 圆体 and SF Rounded carry
# that voice in crisp vector form (Yuanti is a downloadable macOS font,
# fetched by tools/fonts.swift when missing).
YUANTI = next(iter(sorted(Path("/System/Library/AssetsV2").glob(
    "com_apple_MobileAsset_Font*/*.asset/AssetData/Yuanti.ttc"))), None)
ROUND_L, ROUND_R, ROUND_B = (YUANTI, 4), (YUANTI, 0), (YUANTI, 2)
SF_ROUNDED = "/System/Library/Fonts/SFNSRounded.ttf"
LATIN_ROUND = (SF_ROUNDED, 0, 600)          # (path, index, weight axis)
LATIN_ROUND_B = (SF_ROUNDED, 0, 700)
LATIN_ROUND_R = (SF_ROUNDED, 0, 500)

# Palette: the series' bright summer sky. The dialogue keeps the original
# window's deep-teal-to-sky gradient with white letters; the system pages are
# frosted white glass with deep-teal ink and a clear sky accent.
TEAL_TOP, TEAL_BOT = (50, 118, 146), (98, 170, 198)
TEAL_INK = (10, 52, 72)                  # drop shadow under white letters
VIOLET_TOP, VIOLET_BOT = (100, 68, 110), (160, 128, 178)
INK, INK_SUB, INK_FAINT = (24, 66, 92), (86, 124, 146), (140, 170, 188)
LINE, LINE_SOFT = (126, 186, 214), (186, 220, 236)
ACCENT, ACCENT_DEEP = (52, 152, 204), (30, 112, 162)
ACCENT_TOP, ACCENT_BOT, ACCENT_SOFT = (88, 180, 224), (44, 130, 186), (214, 238, 249)
SKY = (112, 196, 232)
WHITE = (247, 251, 253)
CORAL = (255, 128, 110)


# --------------------------------------------------------------------------
# primitives (all coordinates are logical pixels; drawing happens at S)

def rgba(color, alpha=None):
    color = tuple(color)
    if alpha is not None:
        return color[:3] + (int(alpha),)
    return color if len(color) == 4 else color + (255,)


def canvas(size):
    return Image.new("RGBA", (round(size[0] * S), round(size[1] * S)), (0, 0, 0, 0))


def box(coords):
    return tuple(round(v * S) for v in coords)


@lru_cache(maxsize=None)
def font(spec, size):
    path, index = spec[:2]
    if path is None:
        raise FileNotFoundError("Yuanti SC is not installed; run: swift tools/fonts.swift")
    f = ImageFont.truetype(str(path), max(1, round(size * S)), index=index)
    if len(spec) > 2:                       # variable font: (GRAD, Weight)
        f.set_variation_by_axes([400, spec[2]])
    return f


def reduce(master, size):
    """The logical texture: a Lanczos reduction of the 4x master."""
    out = master.resize(size, Image.Resampling.LANCZOS)
    out.putalpha(out.getchannel("A").point(lambda v: 0 if v < 3 else v))
    return out


def vgradient(size, top, bottom, mask=None):
    w, h = round(size[0] * S), round(size[1] * S)
    strip = Image.new("RGBA", (1, h))
    top, bottom = rgba(top), rgba(bottom)
    for y in range(h):
        t = y / max(1, h - 1)
        strip.putpixel((0, y), tuple(round(a + (b - a) * t) for a, b in zip(top, bottom)))
    im = strip.resize((w, h))
    if mask is not None:
        im.putalpha(ImageChops.multiply(im.getchannel("A"), mask))
    return im


def hgradient_line(im, x0, x1, y, color, peak, width=1.0, fade=0.35):
    """A hairline whose alpha rises from 0 to peak and back: a soft highlight."""
    layer = canvas((im.width / S, im.height / S))
    d = ImageDraw.Draw(layer)
    span = max(1, round((x1 - x0) * S))
    for i in range(span):
        t = i / span
        edge = min(t, 1 - t) / fade
        a = peak * min(1.0, edge)
        x = round(x0 * S) + i
        d.line((x, round((y - width / 2) * S), x, round((y + width / 2) * S) - 1), fill=rgba(color, a))
    im.alpha_composite(layer)


def rounded_mask(size, coords, radius):
    mask = Image.new("L", (round(size[0] * S), round(size[1] * S)), 0)
    ImageDraw.Draw(mask).rounded_rectangle(box(coords), radius=round(radius * S), fill=255)
    return mask


def fill_round(im, coords, radius, fill):
    layer = canvas((im.width / S, im.height / S))
    ImageDraw.Draw(layer).rounded_rectangle(box(coords), radius=round(radius * S), fill=rgba(fill))
    im.alpha_composite(layer)


def stroke_round(im, coords, radius, color, width=1.0):
    layer = canvas((im.width / S, im.height / S))
    ImageDraw.Draw(layer).rounded_rectangle(box(coords), radius=round(radius * S),
                                            outline=rgba(color), width=max(1, round(width * S)))
    im.alpha_composite(layer)


def glass(im, coords, radius, top, bottom, border=None, highlight=60):
    """A frosted panel: vertical gradient, hairline border, lit top edge."""
    size = (im.width / S, im.height / S)
    mask = rounded_mask(size, coords, radius)
    im.alpha_composite(vgradient(size, top, bottom, mask))
    if border:
        stroke_round(im, coords, radius, border, 1.0)
    if highlight:
        x0, y0, x1, _ = coords
        hgradient_line(im, x0 + radius * 0.6, x1 - radius * 0.6, y0 + 1.2, (255, 255, 255), highlight, 0.9)


def glow(im, coords, radius, color, blur=6.0, alpha=110):
    layer = canvas((im.width / S, im.height / S))
    ImageDraw.Draw(layer).rounded_rectangle(box(coords), radius=round(radius * S), fill=rgba(color, alpha))
    layer = layer.filter(ImageFilter.GaussianBlur(blur * S))
    im.alpha_composite(layer)


def text(im, xy, value, spec, size, color, anchor="ls", tracking=0.0, shadow=None):
    """Draw text at logical xy. anchor follows PIL ('ls' left-baseline, 'ms', 'rs', 'mm').

    tracking is extra space between characters in logical px. shadow is
    (dx, dy, blur, rgba) and is composited beneath the glyphs.
    """
    f = font(spec, size)
    probe = ImageDraw.Draw(im)
    if tracking:
        widths = [probe.textlength(ch, font=f) for ch in value]
        total = sum(widths) + tracking * S * (len(value) - 1)
        x = xy[0] * S
        if anchor[0] == "m":
            x -= total / 2
        elif anchor[0] == "r":
            x -= total
        glyph_anchor = "l" + anchor[1]
        pieces = []
        for ch, w in zip(value, widths):
            pieces.append((x, ch))
            x += w + tracking * S
    else:
        pieces = [(xy[0] * S, value)]
        glyph_anchor = anchor
    y = xy[1] * S

    def paint(layer, fill, offset=(0, 0)):
        d = ImageDraw.Draw(layer)
        for x, ch in pieces:
            d.text((x + offset[0] * S, y + offset[1] * S), ch, font=f, fill=fill, anchor=glyph_anchor)

    if shadow:
        dx, dy, blur, shade = shadow
        layer = canvas((im.width / S, im.height / S))
        paint(layer, rgba(shade), (dx, dy))
        if blur:
            layer = layer.filter(ImageFilter.GaussianBlur(blur * S))
        im.alpha_composite(layer)
    layer = canvas((im.width / S, im.height / S))
    paint(layer, rgba(color))
    im.alpha_composite(layer)


def text_width(value, spec, size, tracking=0.0):
    f = font(spec, size)
    d = ImageDraw.Draw(Image.new("L", (1, 1)))
    return (sum(d.textlength(ch, font=f) for ch in value) + tracking * S * max(0, len(value) - 1)) / S


def polyline(im, points, color, width):
    layer = canvas((im.width / S, im.height / S))
    ImageDraw.Draw(layer).line([(round(x * S), round(y * S)) for x, y in points], fill=rgba(color),
                               width=max(1, round(width * S)), joint="curve")
    im.alpha_composite(layer)


def polygon(im, points, color):
    layer = canvas((im.width / S, im.height / S))
    ImageDraw.Draw(layer).polygon([(round(x * S), round(y * S)) for x, y in points], fill=rgba(color))
    im.alpha_composite(layer)


def ellipse(im, coords, fill=None, outline=None, width=1.0):
    layer = canvas((im.width / S, im.height / S))
    ImageDraw.Draw(layer).ellipse(box(coords), fill=rgba(fill) if fill else None,
                                  outline=rgba(outline) if outline else None,
                                  width=max(1, round(width * S)))
    im.alpha_composite(layer)


def sheet(cells, cell_size):
    """Concatenate state cells horizontally (each a 4x canvas of cell_size)."""
    out = canvas((cell_size[0] * len(cells), cell_size[1]))
    for i, cell in enumerate(cells):
        out.alpha_composite(cell, (round(i * cell_size[0] * S), 0))
    return out


# --------------------------------------------------------------------------
# icons for the menu bar (11 px glyphs, drawn around a centre)

def icon(im, kind, cx, cy, color, w=1.15):
    c = rgba(color)
    if kind == "qsave":     # arrow into a tray
        polyline(im, [(cx, cy - 5), (cx, cy + 1.5)], c, w)
        polyline(im, [(cx - 3, cy - 1.2), (cx, cy + 1.8), (cx + 3, cy - 1.2)], c, w)
        polyline(im, [(cx - 5, cy + 1.5), (cx - 5, cy + 4.5), (cx + 5, cy + 4.5), (cx + 5, cy + 1.5)], c, w)
    elif kind == "qload":   # arrow out of a tray
        polyline(im, [(cx, cy + 1.5), (cx, cy - 5)], c, w)
        polyline(im, [(cx - 3, cy - 2), (cx, cy - 5), (cx + 3, cy - 2)], c, w)
        polyline(im, [(cx - 5, cy + 1.5), (cx - 5, cy + 4.5), (cx + 5, cy + 4.5), (cx + 5, cy + 1.5)], c, w)
    elif kind == "save":    # bookmark ribbon
        polyline(im, [(cx - 3.6, cy - 5), (cx + 3.6, cy - 5), (cx + 3.6, cy + 5), (cx, cy + 2),
                      (cx - 3.6, cy + 5), (cx - 3.6, cy - 5)], c, w)
    elif kind == "load":    # open book
        polyline(im, [(cx, cy - 3), (cx, cy + 4.6)], c, w)
        polyline(im, [(cx, cy - 3), (cx - 5, cy - 4.4), (cx - 5, cy + 3.2), (cx, cy + 4.6)], c, w)
        polyline(im, [(cx, cy - 3), (cx + 5, cy - 4.4), (cx + 5, cy + 3.2), (cx, cy + 4.6)], c, w)
    elif kind == "config":  # sliders
        for dy, knob in ((-3.3, 1.6), (0.1, -2.2), (3.5, 0.6)):
            polyline(im, [(cx - 5, cy + dy), (cx + 5, cy + dy)], c, w * 0.9)
            ellipse(im, (cx + knob - 1.5, cy + dy - 1.5, cx + knob + 1.5, cy + dy + 1.5), fill=c)
    elif kind == "auto":    # play
        polygon(im, [(cx - 3, cy - 4.4), (cx + 4.4, cy), (cx - 3, cy + 4.4)], c)
    elif kind == "skip":    # fast-forward
        polygon(im, [(cx - 5.2, cy - 4), (cx, cy), (cx - 5.2, cy + 4)], c)
        polygon(im, [(cx - 0.4, cy - 4), (cx + 4.8, cy), (cx - 0.4, cy + 4)], c)
    elif kind == "log":     # lines
        for dy in (-3.6, 0, 3.6):
            polyline(im, [(cx - 3, cy + dy), (cx + 5, cy + dy)], c, w)
            ellipse(im, (cx - 5.6, cy + dy - 0.9, cx - 3.8, cy + dy + 0.9), fill=c)


# --------------------------------------------------------------------------
# message window (FRM_0101 overlay, FRM_0102b / FRM_0106b panels)
#
# The original window is a deep-teal-to-sky gradient with white letters; that
# gradient is the series' signature, so the dialogue keeps it (refined, lit
# glass, rounded). Everything else in the system UI is bright frosted white.

MSG = (750, 152)


def message_panel(top, bottom):
    """The base the script fades with the window-depth setting."""
    im = canvas(MSG)
    mask = rounded_mask(MSG, (0.5, 0.5, 749.5, 151.5), 16)
    im.alpha_composite(vgradient(MSG, rgba(top, 236), rgba(bottom, 244), mask))
    # A soft white haze along the top edge, like light through thin cloud.
    haze = canvas(MSG)
    haze.alpha_composite(vgradient((750, 60), rgba((255, 255, 255), 46), rgba((255, 255, 255), 0)))
    haze.putalpha(ImageChops.multiply(haze.getchannel("A"), mask))
    im.alpha_composite(haze)
    return im


def message_overlay():
    """Above text and buttons (sprite priority 10), so only hairlines live here."""
    im = canvas(MSG)
    stroke_round(im, (0.5, 0.5, 749.5, 151.5), 16, rgba(WHITE, 120), 0.8)
    hgradient_line(im, 30, 720, 1.4, (255, 255, 255), 210, 0.9, fade=0.3)
    # Name rule: a short luminous line that reads as a header with or without a name.
    hgradient_line(im, 22, 330, 36.5, (255, 255, 255), 150, 0.8, fade=0.08)
    hgradient_line(im, 18, 732, 122.5, (255, 255, 255), 64, 0.7, fade=0.25)
    return im


MENU_ITEMS = [  # (texture, label, icon, toggles)
    ("FRM_0117.eri", "快存", "qsave", False),
    ("FRM_0118.eri", "快读", "qload", False),
    ("FRM_0114.eri", "保存", "save", False),
    ("FRM_0113.eri", "读取", "load", False),
    ("FRM_0111.eri", "设定", "config", False),
    ("FRM_0115.eri", "自动", "auto", True),
    ("FRM_0116.eri", "快进", "skip", True),
    ("FRM_0112.eri", "记录", "log", False),
]
MENU_CELL = (84, 25)


def menu_button(label, kind, toggles):
    cells = []
    states = ("normal", "focus", "active") if toggles else ("normal", "focus")
    lw = text_width(label, ROUND_B, 12.0)
    gx = 42 - (lw + 16) / 2          # glyph group: 11 px icon + 5 px gap + label
    for state in states:
        cell = canvas(MENU_CELL)
        if state == "focus":
            fill_round(cell, (4, 2.5, 80, 22.5), 10, rgba((255, 255, 255), 52))
            stroke_round(cell, (4, 2.5, 80, 22.5), 10, rgba((255, 255, 255), 150), 0.7)
        elif state == "active":
            fill_round(cell, (4, 2.5, 80, 22.5), 10, rgba((255, 255, 255), 92))
            stroke_round(cell, (4, 2.5, 80, 22.5), 10, rgba((255, 255, 255), 230), 0.8)
        ink = rgba(WHITE, 222) if state == "normal" else WHITE
        icon(cell, kind, gx + 5.5, 12.6, ink)
        text(cell, (gx + 16, 16.9), label, ROUND_B, 12.0, ink, shadow=(0, 0.6, 0.8, rgba(TEAL_INK, 170)))
        cells.append(cell)
    return sheet(cells, MENU_CELL)


def close_button():
    cells = []
    for state in range(2):
        cell = canvas((16, 17))
        col = rgba(WHITE, 220) if state == 0 else WHITE
        if state:
            ellipse(cell, (0.6, 1.1, 15.4, 15.9), fill=rgba((255, 255, 255), 60))
        polyline(cell, [(4.6, 5.1), (11.4, 11.9)], col, 1.1)
        polyline(cell, [(11.4, 5.1), (4.6, 11.9)], col, 1.1)
        cells.append(cell)
    return sheet(cells, (16, 17))


def voice_button():
    cells = []
    for state in range(2):
        cell = canvas((28, 21))
        col = rgba(WHITE, 225) if state == 0 else rgba(WHITE)
        if state:
            fill_round(cell, (1, 1.5, 27, 19.5), 9, rgba((255, 255, 255), 60))
        polygon(cell, [(7, 8.4), (10, 8.4), (14, 5), (14, 16), (10, 12.6), (7, 12.6)], col)
        for r, a in ((3.6, 255), (6.6, 170)):
            layer = canvas((28, 21))
            ImageDraw.Draw(layer).arc(box((14 - r + 1.2, 10.5 - r, 14 + r + 1.2, 10.5 + r)), start=-50, end=50,
                                      fill=rgba(col, a * col[3] / 255), width=round(1.1 * S))
            cell.alpha_composite(layer)
        cells.append(cell)
    return sheet(cells, (28, 21))


def continue_mark():
    im = canvas((24, 24))
    glow(im, (8, 8, 16, 16), 4, (255, 255, 255), blur=2.4, alpha=110)
    polyline(im, [(7.5, 9.5), (12, 14), (16.5, 9.5)], WHITE, 1.6)
    return im


# --------------------------------------------------------------------------
# choices (FRM_0104: normal | focus, 750x48 each) — bright glass pills, ink text

CHOICE = (750, 48)


def choice_sheet():
    cells = []
    for focus in (False, True):
        cell = canvas(CHOICE)
        coords = (10, 4, 740, 44)
        glow(cell, (14, 9, 736, 45), 18, TEAL_INK, blur=3.0, alpha=46)
        if focus:
            glow(cell, coords, 18, SKY, blur=3.2, alpha=120)
            glass(cell, coords, 18, rgba((236, 248, 254), 250), rgba((205, 234, 248), 252),
                  border=rgba(ACCENT, 255), highlight=255)
        else:
            glass(cell, coords, 18, rgba((255, 255, 255), 236), rgba((238, 247, 252), 240),
                  border=rgba(LINE, 190), highlight=255)
        # Marker: a small diamond, outlined at rest and filled on focus.
        cx, cy, r = 30, 24, 4.4
        pts = [(cx, cy - r), (cx + r, cy), (cx, cy + r), (cx - r, cy)]
        if focus:
            polygon(cell, pts, ACCENT)
        else:
            polyline(cell, pts + [pts[0]], rgba(ACCENT, 230), 1.1)
        cells.append(cell)
    return sheet(cells, CHOICE)


# --------------------------------------------------------------------------
# save / load (FRM_0501 cards, FRM_0502 NEW, FRM_0505/0506 headers)

CARD = (220, 170)
THUMB = (95, 5, 215, 95)    # where the script paints the 120x90 thumbnail


def save_cards():
    cells = []
    for focus in (False, True):
        cell = canvas(CARD)
        coords = (2, 2, 218, 168)
        glow(cell, (5, 6, 215, 168), 12, TEAL_INK, blur=2.4, alpha=40)
        if focus:
            glow(cell, coords, 12, SKY, blur=2.4, alpha=110)
            glass(cell, coords, 12, rgba((255, 255, 255), 252), rgba((226, 243, 251), 252),
                  border=rgba(ACCENT, 255), highlight=255)
        else:
            glass(cell, coords, 12, rgba((255, 255, 255), 240), rgba((238, 247, 252), 244),
                  border=rgba(LINE, 170), highlight=255)
        # Empty-thumbnail well; a saved thumbnail covers it completely.
        tx0, ty0, tx1, ty1 = THUMB
        fill_round(cell, (tx0 - 0.5, ty0 - 0.5, tx1 + 0.5, ty1 + 0.5), 5, rgba((214, 234, 245), 255))
        stroke_round(cell, (tx0 - 0.5, ty0 - 0.5, tx1 + 0.5, ty1 + 0.5), 5, rgba(LINE, 160), 0.7)
        text(cell, ((tx0 + tx1) / 2, (ty0 + ty1) / 2 + 2.6), "NO DATA", LATIN_ROUND_B, 7.4,
             rgba(INK_FAINT, 255), anchor="ms", tracking=1.6)
        hgradient_line(cell, 12, 208, 99.5, ACCENT if focus else LINE, 150 if focus else 120, 0.7, fade=0.2)
        cells.append(cell)
    return sheet(cells, CARD)


def new_badge():
    im = canvas((43, 33))
    glow(im, (5, 9, 38, 24), 7.5, CORAL, blur=2.0, alpha=110)
    fill_round(im, (5, 9, 38, 24), 7.5, rgba(CORAL, 255))
    hgradient_line(im, 10, 33, 10.2, (255, 255, 255), 140, 0.7)
    text(im, (21.5, 19.6), "NEW", LATIN_ROUND_B, 8.2, (255, 255, 255), anchor="ms", tracking=0.9)
    return im


HEADER = (300, 27)


def page_header(save):
    im = canvas(HEADER)
    word, label = ("SAVE", "保存") if save else ("LOAD", "读取")
    # Over any scene (bright title sky or a dark CG): white with a soft teal drop.
    halo = (0, 0.8, 1.6, rgba(TEAL_INK, 170))
    text(im, (4, 21.5), word, LATIN_ROUND_B, 19, (255, 255, 255), tracking=3.2, shadow=halo)
    w = text_width(word, LATIN_ROUND_B, 19, 3.2)
    text(im, (w + 14, 21), label, ROUND_B, 12.5, (255, 255, 255), shadow=halo)
    x = w + 14 + text_width(label, ROUND_B, 12.5) + 12
    hgradient_line(im, x, 300, 15.5, (255, 255, 255), 200, 0.9, fade=0.02)
    return im


# --------------------------------------------------------------------------
# history (FRM_0301 panel, FRM_0302 track, FRM_0303 thumb) — the dialogue
# text style is shared with the message window, so the log keeps its teal glass

def history_panel():
    size = (701, 504)
    im = canvas(size)
    glow(im, (4, 6, 697, 502), 14, TEAL_INK, blur=3.0, alpha=60)
    glass(im, (0.5, 0.5, 700.5, 503.5), 14, rgba(TEAL_TOP, 246), rgba(TEAL_BOT, 250),
          border=rgba(WHITE, 130), highlight=210)
    for y in (123, 251, 379):
        hgradient_line(im, 16, 685, y + 0.5, (255, 255, 255), 80, 0.7, fade=0.08)
    return im


def history_track():
    size = (18, 488)
    im = canvas(size)
    fill_round(im, (7.6, 4, 10.4, 484), 1.4, rgba(ACCENT, 70))
    return im


def history_thumb():
    size = (54, 18)
    im = canvas(size)
    for i, (fill, a) in enumerate(((ACCENT, 190), (ACCENT, 240), (ACCENT_DEEP, 255))):
        x = i * 18
        fill_round(im, (x + 6.2, 1.5, x + 11.8, 16.5), 2.8, rgba(fill, a))
    return im


# --------------------------------------------------------------------------
# confirm dialog (FRM_0401 800x120, FRM_0402/0403 yes/no)

def confirm_panel():
    size = (800, 120)
    im = canvas(size)
    coords = (168, 4, 632, 116)
    glow(im, (172, 10, 628, 118), 18, TEAL_INK, blur=5.0, alpha=80)
    glass(im, coords, 18, rgba((255, 255, 255), 252), rgba((234, 246, 252), 254),
          border=rgba(LINE, 200), highlight=255)
    return im


def confirm_button(label, primary):
    cells = []
    for focus in (False, True):
        cell = canvas((82, 25))
        coords = (2, 1.5, 80, 23.5)
        if primary or focus:
            if focus:
                glow(cell, coords, 11, SKY, blur=1.6, alpha=120)
            glass(cell, coords, 11, rgba(ACCENT_TOP, 255), rgba(ACCENT_BOT, 255),
                  border=rgba((255, 255, 255), 200 if focus else 120), highlight=160)
            ink = (255, 255, 255)
        else:
            fill_round(cell, coords, 11, rgba((255, 255, 255), 255))
            stroke_round(cell, coords, 11, rgba(LINE, 230), 0.8)
            ink = INK
        text(cell, (41, 17), label, ROUND_B, 12.5, ink, anchor="ms")
        cells.append(cell)
    return sheet(cells, (82, 25))


# --------------------------------------------------------------------------
# settings window (FRM_0201 panel, option chips, sliders, voice details)

CONFIG = (532, 552)
# (label, caption, control centre y, label x)
CONFIG_ROWS = {
    "yosuga": [("显示模式", "SCREEN MODE", 36.5, 34), ("音乐", "MUSIC", 89.5, 34),
               ("效果音", "SOUND EFFECT", 116.5, 34), ("系统音", "SYSTEM SOUND", 143.5, 34),
               ("语音", "VOICE", 170.5, 34), ("画面效果", "SCREEN EFFECT", 239.5, 34),
               ("窗口透明度", "WINDOW OPACITY", 291.5, 34), ("文章显示速度", "TEXT SPEED", 318.5, 34),
               ("文章快进", "SKIP", 340.5, 34), ("点击时的声音控制", "VOICE ON CLICK", 367.5, 34),
               ("文章前进速度", "AUTO SPEED", 473.5, 34)],
    "haruka": [("画面模式", "SCREEN MODE", 36.5, 34), ("音乐", "MUSIC", 89.5, 34),
               ("效果音", "SOUND EFFECT", 116.5, 34), ("系统音", "SYSTEM SOUND", 143.5, 34),
               ("语音", "VOICE", 170.5, 34), ("画面效果", "SCREEN EFFECT", 239.5, 34),
               ("对话框透明度", "WINDOW OPACITY", 291.5, 34), ("文字显示速度", "TEXT SPEED", 318.5, 34),
               ("文字快进", "SKIP", 340.5, 34), ("点击时音效控制", "VOICE ON CLICK", 367.5, 34),
               ("文字播放速度", "AUTO SPEED", 473.5, 34)],
}
CONFIG_GROUPS = [  # card extents (top, bottom) and row separators
    (16, 57, []),
    (66, 212, [103, 130, 157]),
    (220, 259, []),
    (268, 384, [305, 329.5, 354]),
    (414, 492, [452]),
]
CONFIG_MARKERS = [  # (left, right, row y, slider x0, slider x1)
    ("MIN", "MAX", 89.5, 256, 474), ("MIN", "MAX", 116.5, 256, 474),
    ("MIN", "MAX", 143.5, 256, 474), ("MIN", "MAX", 170.5, 256, 474),
    ("LIGHT", "DEEP", 291.5, 250, 468), ("FAST", "SLOW", 318.5, 250, 468),
    ("FAST", "SLOW", 473.5, 250, 468),
]
AUTO_HEADER = {"yosuga": "自动模式设定", "haruka": "自动模式设定"}


def row_label(im, x, cy, label, caption=None, size=13.4):
    """A settings row title with its English caption inline, centred on the control row."""
    base = cy + size * 0.36
    text(im, (x, base), label, ROUND_B, size, INK)
    if caption:
        text(im, (x + text_width(label, ROUND_B, size) + 6, base - 0.6), caption, LATIN_ROUND_B, 5.6,
             rgba(ACCENT, 255), tracking=1.1)


def config_panel(game):
    im = canvas(CONFIG)
    glow(im, (8, 10, 524, 548), 18, TEAL_INK, blur=5.0, alpha=70)
    glass(im, (2, 2, 530, 550), 18, rgba((255, 255, 255), 250), rgba((234, 246, 252), 252),
          border=rgba(LINE, 200), highlight=255)
    for top, bottom, separators in CONFIG_GROUPS:
        fill_round(im, (16, top, 516, bottom), 10, rgba((240, 248, 252), 255))
        stroke_round(im, (16, top, 516, bottom), 10, rgba(LINE_SOFT, 255), 0.7)
        for y in separators:
            hgradient_line(im, 28, 504, y, LINE, 120, 0.6, fade=0.04)
    for label, caption, cy, x in CONFIG_ROWS[game]:
        if label in ("文章前进速度", "文字播放速度"):
            x = 46
        row_label(im, x, cy, label, caption)
    # The auto-mode block is introduced by a small section title.
    text(im, (34, 437.5), AUTO_HEADER[game], ROUND_B, 11.5, ACCENT_DEEP)
    text(im, (34 + text_width(AUTO_HEADER[game], ROUND_B, 11.5) + 8, 437.2), "AUTO MODE", LATIN_ROUND_B, 5.6,
         rgba(ACCENT, 255), tracking=1.1)
    for left, right, cy, x0, x1 in CONFIG_MARKERS:
        text(im, (x0 - 6, cy + 2.2), left, LATIN_ROUND_B, 5.8, INK_SUB, anchor="rs", tracking=0.7)
        text(im, (x1 + 6, cy + 2.2), right, LATIN_ROUND_B, 5.8, INK_SUB, anchor="ls", tracking=0.7)
    return im


OPTION_LABELS = {
    "FRM_0202.eri": "窗口", "FRM_0203.eri": "全屏", "FRM_0205.eri": "正常", "FRM_0206.eri": "无",
    "FRM_0207.eri": "播放", "FRM_0208.eri": "停止", "FRM_0209.eri": "已读", "FRM_0210.eri": "全部",
    "FRM_0211.eri": "继续", "FRM_0212.eri": "停止",
}
ACTION_LABELS = {
    "FRM_0204.eri": "个别设定", "FRM_0214.eri": "初始化设定", "FRM_0215.eri": "返回标题",
    "FRM_0216.eri": "结束游戏", "FRM_0218.eri": "返回鉴赏",
}


def option_chip(label):
    """106x23 per state: off, hover, selected (a segmented-control look)."""
    cells = []
    for state in range(3):
        cell = canvas((106, 23))
        coords = (3, 2, 103, 21)
        if state == 0:
            fill_round(cell, coords, 9.5, rgba((255, 255, 255), 255))
            stroke_round(cell, coords, 9.5, rgba(LINE, 200), 0.7)
            ink = INK_SUB
        elif state == 1:
            fill_round(cell, coords, 9.5, rgba(ACCENT_SOFT, 255))
            stroke_round(cell, coords, 9.5, rgba(ACCENT, 255), 0.8)
            ink = INK
        else:
            glow(cell, coords, 9.5, SKY, blur=1.4, alpha=110)
            glass(cell, coords, 9.5, rgba(ACCENT_TOP, 255), rgba(ACCENT_BOT, 255),
                  border=rgba((255, 255, 255), 200), highlight=170)
            ink = (255, 255, 255)
        text(cell, (53, 15.7), label, ROUND_B, 12, ink, anchor="ms")
        cells.append(cell)
    return sheet(cells, (106, 23))


def action_button(label):
    """129x26 per state: normal, focus, pressed."""
    cells = []
    for state in range(3):
        cell = canvas((129, 26))
        coords = (2.5, 2, 126.5, 24)
        if state == 0:
            fill_round(cell, coords, 11, rgba((255, 255, 255), 255))
            stroke_round(cell, coords, 11, rgba(LINE, 220), 0.8)
            ink = INK
        elif state == 1:
            glow(cell, coords, 11, SKY, blur=1.4, alpha=80)
            fill_round(cell, coords, 11, rgba(ACCENT_SOFT, 255))
            stroke_round(cell, coords, 11, rgba(ACCENT, 255), 0.9)
            ink = INK
        else:
            glass(cell, coords, 11, rgba(ACCENT_TOP, 255), rgba(ACCENT_BOT, 255),
                  border=rgba((255, 255, 255), 200), highlight=170)
            ink = (255, 255, 255)
        text(cell, (64.5, 17.6), label, ROUND_B, 12.2, ink, anchor="ms")
        cells.append(cell)
    return sheet(cells, (129, 26))


def sound_toggle():
    """37x27 per state: off, off-focus, on, on-focus (speaker glyph)."""
    cells = []
    for state in range(4):
        cell = canvas((37, 27))
        on, focus = state >= 2, state % 2 == 1
        if focus:
            ellipse(cell, (5, 0.5, 31, 26.5), fill=rgba(ACCENT_SOFT, 255))
        col = (ACCENT_DEEP if focus else ACCENT) if on else (INK_SUB if focus else INK_FAINT)
        polygon(cell, [(10, 10.6), (14, 10.6), (19, 6.4), (19, 20.6), (14, 16.4), (10, 16.4)], col)
        if on:
            for r, a in ((3.8, 255), (7.0, 175)):
                layer = canvas((37, 27))
                ImageDraw.Draw(layer).arc(box((19.6 - r, 13.5 - r, 19.6 + r, 13.5 + r)), start=-52, end=52,
                                          fill=rgba(col, a), width=round(1.25 * S))
                cell.alpha_composite(layer)
        else:
            polyline(cell, [(22.4, 10.8), (27.6, 16.2)], col, 1.15)
            polyline(cell, [(27.6, 10.8), (22.4, 16.2)], col, 1.15)
        cells.append(cell)
    return sheet(cells, (37, 27))


def slider_track():
    size = (202, 13)
    im = canvas(size)
    fill_round(im, (1, 5.0, 201, 8.0), 1.5, rgba((206, 228, 240), 255))
    stroke_round(im, (1, 5.0, 201, 8.0), 1.5, rgba(LINE, 150), 0.5)
    return im


def slider_knob():
    cells = []
    for focus in (False, True):
        cell = canvas((13, 13))
        if focus:
            ellipse(cell, (0.2, 0.2, 12.8, 12.8), fill=rgba(SKY, 110))
        ellipse(cell, (2, 2.6, 11, 11.6), fill=rgba(TEAL_INK, 70))
        ellipse(cell, (2, 1.6, 11, 10.6), fill=(255, 255, 255, 255),
                outline=rgba(ACCENT, 255), width=1.0)
        ellipse(cell, (4.6, 4.2, 8.4, 8.0), fill=rgba(ACCENT, 255 if focus else 210))
        cells.append(cell)
    return sheet(cells, (13, 13))


VOICE_NAMES = {  # (name, romanisation, toggle row centre y, label x); Yosuga's art writes 渚一葉
    "yosuga": [("春日野穹", "SORA KASUGANO", 26.5, 16), ("天女目瑛", "AKIRA AMATSUME", 26.5, 178),
               ("依媛奈绪", "NAO YORIHIME", 56.5, 16), ("渚一葉", "KAZUHA MIGIWA", 56.5, 178),
               ("乃木坂初佳", "MOTOKA NOGISAKA", 86.5, 16), ("仓永梢", "KOZUE KURANAGA", 86.5, 178),
               ("伊福部八寻", "YAHIRO IFUKUBE", 116.5, 16), ("中里亮平", "RYOUHEI NAKAZATO", 116.5, 178),
               ("其他", "OTHERS", 146.5, 16)],
}
VOICE_NAMES["haruka"] = [(n.replace("渚一葉", "渚一叶"), r, y, x) for n, r, y, x in VOICE_NAMES["yosuga"]]


def voice_details_panel(game):
    size = (335, 179)
    im = canvas(size)
    glow(im, (5, 7, 330, 176), 14, TEAL_INK, blur=3.6, alpha=80)
    glass(im, (1.5, 1.5, 333.5, 177.5), 14, rgba((255, 255, 255), 252), rgba((234, 246, 252), 254),
          border=rgba(LINE, 210), highlight=255)
    for y in (41.5, 71.5, 101.5, 131.5):
        hgradient_line(im, 12, 323, y, LINE, 120, 0.6, fade=0.04)
    for name, roman, cy, x in VOICE_NAMES[game]:
        text(im, (x, cy + 0.2), name, ROUND_B, 12.2, INK)
        text(im, (x + 0.5, cy + 9.4), roman, LATIN_ROUND_B, 5.0, rgba(ACCENT, 255), tracking=0.9)
    return im


# --------------------------------------------------------------------------
# assembly

def frame_masters(game):
    """name in the archive -> 4x master. Sizes are checked by the caller."""
    m = {
        "FRM_0101.eri": message_overlay(),
        "FRM_0102b.eri": message_panel(TEAL_TOP, TEAL_BOT),
        "FRM_0106b.eri": message_panel(VIOLET_TOP, VIOLET_BOT),
        "FRM_0103.eri": continue_mark(),
        "FRM_0104.eri": choice_sheet(),
        "FRM_0105.eri": voice_button(),
        "FRM_0119.eri": close_button(),
        "FRM_0501.eri": save_cards(),
        "FRM_0502.eri": new_badge(),
        "FRM_0505.eri": page_header(False),
        "FRM_0506.eri": page_header(True),
        "FRM_0301.eri": history_panel(),
        "FRM_0302.eri": history_track(),
        "FRM_0303.eri": history_thumb(),
        "FRM_0401.eri": confirm_panel(),
        "FRM_0402.eri": confirm_button("是", True),
        "FRM_0403.eri": confirm_button("否", False),
        "FRM_0201.eri": config_panel(game),
        "FRM_0217.eri": sound_toggle(),
        "FRM_0221.eri": slider_track(),
        "FRM_0222.eri": slider_knob(),
        "FRM_0231.eri": voice_details_panel(game),
    }
    for name, label, kind, toggles in MENU_ITEMS:
        m[name] = menu_button(label, kind, toggles)
    for name, label in OPTION_LABELS.items():
        m[name] = option_chip(label)
    for name, label in ACTION_LABELS.items():
        m[name] = action_button(label)
    return m


# Logical sizes that differ from the original artwork on purpose.
RESIZED = {
    "FRM_0505.eri": HEADER, "FRM_0506.eri": HEADER,
    "FRM_0115.eri": (252, 25), "FRM_0116.eri": (252, 25),
}


def _style_block(xml, style_id):
    start = xml.index(f'<style id="{style_id}"')
    end = xml.index("</style>", start) + len("</style>")
    return start, end


def _edit_style(xml, style_id, replacements):
    start, end = _style_block(xml, style_id)
    block = xml[start:end]
    for old, new, count in replacements:
        found = block.count(old)
        if found != count:
            raise ValueError(f"{style_id}: expected {count} of {old!r}, found {found}")
        block = block.replace(old, new)
    return xml[:start] + block + xml[end:]


def _replace(xml, old, new, count=1):
    found = xml.count(old)
    if found != count:
        raise ValueError(f"expected {count} of {old!r}, found {found}")
    return xml.replace(old, new)


GOTHIC = 'face="ＭＳ&#32;ゴシック"'
MINCHO = 'face="ＭＳ&#32;明朝"'


def color(rgb):
    """A skin colour attribute. Written in decimal: the SDK's rich-integer
    parser reads a hex value containing the digit E (00C4EAF8H) as an
    exponent and yields black."""
    r, g, b = rgb
    return str(r << 16 | g << 8 | b)


def text_tag(rgb):
    return f'<text color="{color(rgb)}" transparency="0"/>'


def shadow_tag(rgb, x=0, y=1, visible=True):
    return f'<shadow color="{color(rgb)}" transparency="{0 if visible else 256}" x="{x}" y="{y}"/>'


def frame_xml(xml):
    """Typography and layout edits. Every edit asserts exactly what it touches."""
    old_white = '<text color="00FFFFFFH" transparency="0"/>'
    # Dialogue and names: white letters with a quiet deep-teal drop on the teal glass.
    xml = _edit_style(xml, "ID_FONT_NAME", [
        ('line_height="30"', 'line_height="28"', 1),
        (f'bold="false" {GOTHIC} italic="false" size="32"', f'bold="true" {GOTHIC} italic="false" size="22"', 1),
        ('<shadow color="00008080H" transparency="0" x="1" y="2"/>', shadow_tag(TEAL_INK), 1)])
    xml = _edit_style(xml, "ID_FONT_MESSAGE", [
        ('<shadow color="00004080H" transparency="0" x="1" y="2"/>', shadow_tag(TEAL_INK), 1)])
    # Choices: upright rounded-gothic ink on the bright pills, room for the marker.
    xml = _edit_style(xml, "ID_BTN_SELECT_H", [
        (f'bold="true" {MINCHO} italic="true" size="-27"', f'bold="true" {GOTHIC} italic="false" size="21"', 6),
        ('left="6" line_height="22" top="8" width="512"', 'left="48" line_height="24" top="13" width="660"', 6),
        (old_white, text_tag(INK), 6),
        ('<shadow color="21599EH" transparency="0" x="1" y="2"/>', shadow_tag(INK, visible=False), 3),
        ('<shadow color="0021599EH" transparency="0" x="1" y="2"/>', shadow_tag(INK, visible=False), 3)])
    xml = _edit_style(xml, "ID_FONT_SELECT", [
        (f'bold="true" {MINCHO} italic="false" size="32"', f'bold="true" {GOTHIC} italic="false" size="21"', 1),
        ('line_height="34"', 'line_height="24"', 1),
        (old_white, text_tag(INK), 1),
        ('<shadow color="0021599EH" transparency="0" x="1" y="2"/>', shadow_tag(INK, visible=False), 1)])
    xml = _replace(xml, '<static_text height="36" id="ID_TEXT" style="ID_FONT_SELECT" text="ほげほげ" width="565" x="6" y="8"/>',
                   '<static_text height="36" id="ID_TEXT" style="ID_FONT_SELECT" text="ほげほげ" width="660" x="48" y="12"/>')
    # Menu bar: evenly spaced; auto/skip get a distinct "on" state.
    old_x = ["0", "85", "170", "255", "366", "478", "563", "648"]
    new_x = ["0", "92", "184", "276", "368", "460", "552", "644"]
    ids = ["ID_QSAVE", "ID_QLOAD", "ID_SAVE", "ID_LOAD", "ID_CONFIG", "ID_AUTO", "ID_SKIP", "ID_HISTORY"]
    start = xml.index('id="ID_PAGE_ADV_MENU"')
    end = xml.index("</page>", start)
    page = xml[start:end]
    for ident, old, new in zip(ids, old_x, new_x):
        page = _replace(page, f'id="{ident}" style="ID_BTN_ADV_{ident[3:]}" width="0" x="{old}" y="0"/>',
                        f'id="{ident}" style="ID_BTN_ADV_{ident[3:]}" width="0" x="{new}" y="0"/>')
    xml = xml[:start] + page + xml[end:]
    for style, image in (("ID_BTN_ADV_AUTO", "ID_FRM_0115"), ("ID_BTN_ADV_SKIP", "ID_FRM_0116")):
        start, end = _style_block(xml, style)
        block = xml[start:end]
        for state in ("pushed", "pushed_focus", "push_disabled"):
            block = _replace(block, f'<{state} image="{image}:RECT(84,0,84,25)">',
                             f'<{state} image="{image}:RECT(168,0,84,25)">')
        xml = xml[:start] + block + xml[end:]
    # Save cards: number top-left, NEW under it, quieter date, two-line comment, ink on white.
    start = xml.index('id="ID_PAGE_SAVE_INDEX"')
    end = xml.index("</page>", start)
    page = xml[start:end]
    page = _replace(page, '<image id="ID_NEW" rsrc="ID_FRM_0502" x="8" y="3"/>',
                    '<image id="ID_NEW" rsrc="ID_FRM_0502" x="9" y="42"/>')
    page = _replace(page, 'id="ID_NUMBER" style="ID_FONT_LOADSAVE_NUM" text="９９" width="72" x="15" y="38"/>',
                    'id="ID_NUMBER" style="ID_FONT_LOADSAVE_NUM" text="９９" width="80" x="14" y="12"/>')
    page = _replace(page, '<static_text height="20" id="ID_DATE" style="ID_FONT_LOADSAVE_INFO" text="9999/99/99&#32;99:99:99" width="200" x="7" y="100"/>',
                    '<static_text height="20" id="ID_DATE" style="ID_FONT_LOADSAVE_DATE" text="9999/99/99&#32;99:99:99" width="200" x="13" y="105"/>')
    page = _replace(page, 'id="ID_COMMENT" style="ID_FONT_LOADSAVE_INFO" text="ほげほげほげほげほげ" width="196" x="18" y="121"/>',
                    'id="ID_COMMENT" style="ID_FONT_LOADSAVE_INFO" text="ほげほげほげほげほげ" width="196" x="13" y="125"/>')
    xml = xml[:start] + page + xml[end:]
    xml = _edit_style(xml, "ID_FONT_LOADSAVE_NUM", [
        ('line_height="32"', 'line_height="26"', 1),
        (f'bold="true" {GOTHIC} italic="false" size="32"', f'bold="true" {GOTHIC} italic="false" size="24"', 1),
        (old_white, text_tag(INK), 1),
        ('<shadow color="00004080H" transparency="0" x="1" y="1"/>', shadow_tag(INK, visible=False), 1)])
    start, end = _style_block(xml, "ID_FONT_LOADSAVE_INFO")
    info = xml[start:end]
    info_new = (info.replace('line_height="18"', 'line_height="19"')
                    .replace(f'bold="true" {GOTHIC} italic="false" size="17"', f'bold="false" {GOTHIC} italic="false" size="14"')
                    .replace(old_white, text_tag(INK))
                    .replace('<shadow color="0021599EH" transparency="0" x="1" y="2"/>', shadow_tag(INK, visible=False)))
    if info_new.count('size="14"') != 1 or info_new.count('transparency="256"') != 1 or info_new.count(color(INK)) != 2:
        raise ValueError("ID_FONT_LOADSAVE_INFO did not take the expected edits")
    date = (info_new.replace('id="ID_FONT_LOADSAVE_INFO"', 'id="ID_FONT_LOADSAVE_DATE"')
                    .replace('size="14"', 'size="12"')
                    .replace(text_tag(INK), text_tag(INK_SUB)))
    xml = xml[:start] + info_new + "\r\n\t\t" + date + xml[end:]
    # Confirmations: ink on the white card.
    xml = _edit_style(xml, "ID_FONT_CONFIRM", [
        (f'bold="false" {GOTHIC} italic="false" size="21"', f'bold="false" {GOTHIC} italic="false" size="19"', 1),
        (old_white, text_tag(INK), 1),
        ('<shadow color="00004040H" transparency="0" x="1" y="2"/>', shadow_tag(INK, visible=False), 1)])
    return xml


# --------------------------------------------------------------------------
# titles: menu buttons in both games

TITLE_BUTTONS = {
    "yosuga": {
        "FRM_0621.eri": ("开始", "GAME START"), "FRM_0622.eri": ("继续", "CONTINUE"),
        "FRM_0623.eri": ("设定", "CONFIG"), "FRM_0624.eri": ("鉴赏", "APPRECIATION"),
        "FRM_0625.eri": ("结束", "GAME END"), "FRM_0626.eri": ("八寻的并不特别的一天", "YAHIRO SCENARIO"),
    },
    "haruka": {
        "FRM_0621.eri": ("短剧", "SHORT SCENARIO"), "FRM_0622.eri": ("继续", "CONTINUATION"),
        "FRM_0623.eri": ("设定", "CONFIG"), "FRM_0624.eri": ("鉴赏", "APPRECIATION"),
        "FRM_0625.eri": ("返回缘之空", "YOSUGA NO SORA"), "FRM_0626.eri": ("WEB 内容剧场", "WEB CONTENTS"),
        "FRM_0627.eri": ("壁纸鉴赏", "WALLPAPER"),
        "FRM_0631.eri": ("顽固的班长也会恋爱", None), "FRM_0632.eri": ("不合的二人？", None),
        "FRM_0633.eri": ("在苍穹的尽头", None), "FRM_0634.eri": ("让我听听你的歌", None),
        "FRM_0641.eri": ("倒计时", None), "FRM_0642.eri": ("梢之空", None),
        "FRM_0643.eri": ("第一集《那个女孩是巫女！》", None), "FRM_0644.eri": ("第二集《游泳部只有三个部员》", None),
        "FRM_0645.eri": ("第三集《希望你能听我说！》", None), "FRM_0646.eri": ("第四集《大小姐的品格》", None),
        "FRM_0647.eri": ("第五集《女仆小姐加油啊！》", None), "FRM_0648.eri": ("特别篇《班长！发售日到了啊！》", None),
    },
}
TITLE_INK = (14, 62, 92)
TITLE_GUIDE = ("攻略进度", "ROUTE MAP")
# Yosuga's buttons grow from 127x24 to the menu page's full width so the
# label and its caption have room and the whole row is clickable.
YOSUGA_TITLE_CELL = (178, 28)


def title_button(size, label, caption):
    """At rest: rounded ink letters with a white halo on the sky. Focus: a frosted capsule."""
    w, h = size
    tall = h >= 30
    label_size = 16.5 if tall else 15.0
    caption_size = 6.0
    track = 0.8 if len(label) <= 4 else 0.2
    if caption:
        label_y = h / 2 + (2.2 if tall else 1.8)
        cap_y = h - (3.6 if tall else 3.2)
    else:
        label_y = h / 2 + label_size * 0.36
        cap_y = None
    content = max(text_width(label, ROUND_B, label_size, track),
                  text_width(caption, LATIN_ROUND, caption_size, 1.5) if caption else 0)
    pill_w = min(w - 2, content + 52)
    x0, x1 = (w - pill_w) / 2, (w + pill_w) / 2
    cells = []
    for focus in (False, True):
        cell = canvas(size)
        if focus:
            glow(cell, (x0 + 3, 3, x1 - 3, h - 3), (h - 6) / 2, (255, 255, 255), blur=3.0, alpha=150)
            fill_round(cell, (x0 + 1, 1.5, x1 - 1, h - 1.5), (h - 3) / 2, rgba((255, 255, 255), 226))
            stroke_round(cell, (x0 + 1, 1.5, x1 - 1, h - 1.5), (h - 3) / 2, rgba(SKY, 215), 0.8)
            for cx in (x0 + 12, x1 - 12):
                ellipse(cell, (cx - 2.1, h / 2 - 2.1, cx + 2.1, h / 2 + 2.1), fill=rgba(SKY, 255))
            ink, cap, halo = (16, 104, 150), (52, 150, 192), None
        else:
            ink, cap = TITLE_INK, (46, 118, 150)
            halo = (0, 0, 1.8, rgba((255, 255, 255), 235))
        text(cell, (w / 2, label_y), label, ROUND_B, label_size, ink, anchor="ms", tracking=track, shadow=halo)
        if caption:
            text(cell, (w / 2, cap_y), caption, LATIN_ROUND, caption_size, cap, anchor="ms", tracking=1.5,
                 shadow=halo)
        cells.append(cell)
    return sheet(cells, size)


def title_masters(game, sizes):
    """sizes: archive name -> logical sheet size (two states side by side)."""
    out = {}
    for name, (label, caption) in TITLE_BUTTONS[game].items():
        w, h = sizes[name]
        out[name] = title_button((w // 2, h), label, caption)
    return out


def title_mask(size):
    """All-white hit mask: the whole cell is clickable (like the originals)."""
    return Image.new("RGBA", size, (255, 255, 255, 255))


GUIDE_TEXTURE = "add_title_guide.png"
TITLE_MASK = "add_title_mask.png"


def _page_bounds(xml, page_id):
    start = xml.index(f'id="{page_id}"')
    return xml.rindex("<page", 0, start), xml.index("</page>", start)


def title_xml(game, xml):
    """Title edits: Yosuga's wider buttons on a matching mask, and a 攻略进度 entry.

    The ID_GUIDE command is handled by the host (Bridge::FilterCommand), which
    opens the in-game route map; the original Title() never sees it.
    """
    CRLF = "\r\n"
    if game == "yosuga":
        cw, ch = YOSUGA_TITLE_CELL
        for style in ("START", "CONTINUE", "CONFIG", "APPRECIATION", "EXITGAME"):
            start, end = _style_block(xml, f"ID_BTN_TITLE_{style}")
            block = xml[start:end]
            block = _replace(block, ":RECT(0,0,127,24)", f":RECT(0,0,{cw},{ch})", 1)
            block = _replace(block, ":RECT(127,0,127,24)", f":RECT({cw},0,{cw},{ch})", 2)
            block = block.replace('<mask image="ID_FRM_0621_"/>', '<mask image="ID_SKY_TITLE_MASK"/>')
            xml = xml[:start] + block + xml[end:]
        if xml.count("ID_SKY_TITLE_MASK") != 5:
            raise ValueError("every Yosuga menu style should use the new mask")
        layout = {"ID_PAGE_MENU": ["ID_START", "ID_CONTINUE", "ID_CONFIG", "ID_GUIDE", "ID_EXITGAME"],
                  "ID_PAGE_MENU_FULL": ["ID_START", "ID_CONTINUE", "ID_CONFIG", "ID_APPRECIATION", "ID_GUIDE",
                                        "ID_EXITGAME"]}
        pitch, gw, gh, gx = 31, cw, ch, 0
        open_tag, close = _page_bounds(xml, "ID_PAGE_MENU_TG")
        tg = xml[open_tag:close]
        for ident in ("ID_CONFIG", "ID_EXITGAME"):
            tg = _replace(tg, f'id="{ident}" style="ID_BTN_TITLE_{ident[3:]}" width="0" x="25"',
                          f'id="{ident}" style="ID_BTN_TITLE_{ident[3:]}" width="0" x="0"')
        xml = xml[:open_tag] + tg + xml[close:]
    else:
        layout = {"ID_PAGE_MENU_FULL": ["ID_SHORTSCENARIO", "ID_WEB", "ID_WALLPAPER", "ID_CONTINUE", "ID_CONFIG",
                                        "ID_APPRECIATION", "ID_GUIDE", "ID_EXITGAME"]}
        pitch, gw, gh, gx = 40, 198, 31, 2
    mask_w, mask_h = (cw, ch) if game == "yosuga" else (gw, gh)
    xml = xml.replace("\t</resource>",
                      f'\t\t<image id="ID_SKY_TITLE_GUIDE" src="{GUIDE_TEXTURE}"/>{CRLF}'
                      f'\t\t<image id="ID_SKY_TITLE_MASK" src="{TITLE_MASK}"/>{CRLF}\t</resource>', 1)
    style = (f'\t\t<style id="ID_BTN_TITLE_GUIDE" type="button">{CRLF}'
             f'\t\t\t<arrange type="button"/>{CRLF}'
             f'\t\t\t<normal image="ID_SKY_TITLE_GUIDE:RECT(0,0,{gw},{gh})"/>{CRLF}'
             f'\t\t\t<pushed image="ID_SKY_TITLE_GUIDE:RECT({gw},0,{gw},{gh})"/>{CRLF}'
             f'\t\t\t<focus image="ID_SKY_TITLE_GUIDE:RECT({gw},0,{gw},{gh})"/>{CRLF}'
             f'\t\t\t<mask image="ID_SKY_TITLE_MASK"/>{CRLF}'
             f'\t\t</style>{CRLF}')
    xml = xml.replace("\t</declare_style>", style + "\t</declare_style>", 1)
    for page_id, order in layout.items():
        open_tag, close = _page_bounds(xml, page_id)
        page = xml[open_tag:close]
        lines = page.split(CRLF)
        buttons = {}
        for line in lines:
            if "<button " in line:
                ident = line.split('id="')[1].split('"')[0]
                buttons[ident] = line
        missing = [b for b in order if b != "ID_GUIDE" and b not in buttons]
        if missing or len(buttons) != len(order) - 1:
            raise ValueError(f"{game} {page_id}: unexpected buttons {sorted(buttons)}")
        rebuilt = []
        for i, ident in enumerate(order):
            y = i * pitch
            if ident == "ID_GUIDE":
                rebuilt.append(f'\t\t<button group="true" height="0" id="ID_GUIDE" style="ID_BTN_TITLE_GUIDE" '
                               f'width="0" x="{gx}" y="{y}"/>')
                continue
            line = buttons[ident]
            line = line.split(' x="')[0] + (f' x="{gx}"' if game == "yosuga" else ' x="' + line.split(' x="')[1].split('"')[0] + '"') + f' y="{y}"/>'
            rebuilt.append(line)
        head = lines[0]
        old_h = head.split('height="')[1].split('"')[0]
        head = head.replace(f'height="{old_h}"', f'height="{(len(order) - 1) * pitch + (gh if game == "yosuga" else 31) + 2}"', 1)
        body = [head] + rebuilt + [lines[-1]]
        xml = xml[:open_tag] + CRLF.join(body) + xml[close:]
    return xml


# Yosuga's menu sheets (two states side by side) after the resize.
TITLE_RESIZED = {"yosuga": {f"FRM_062{i}.eri": (YOSUGA_TITLE_CELL[0] * 2, YOSUGA_TITLE_CELL[1]) for i in range(1, 6)},
                 "haruka": {}}


def title_new_textures(game):
    """New PNG entries for the title archive: archive name -> (4x master or None, image)."""
    size = YOSUGA_TITLE_CELL if game == "yosuga" else (198, 31)
    master = title_button(size, *TITLE_GUIDE)
    return {GUIDE_TEXTURE: master}, {TITLE_MASK: title_mask(size)}


# --------------------------------------------------------------------------
# appreciation (option.noa): gallery header, character tabs, page tabs, CG and
# recollection grids, the music list and the CG viewer's scroll bar

MARU = ("/System/Library/Fonts/ヒラギノ丸ゴ ProN W4.ttc", 1)   # rounded Japanese, for track titles
OPTION_TAGS = {
    "yosuga": {"FRM_0711.eri": ("春日野穹", "SORA KASUGANO"), "FRM_0712.eri": ("依媛奈绪", "NAO YORIHIME"),
               "FRM_0713.eri": ("天女目瑛", "AKIRA AMATSUME"), "FRM_0714.eri": ("渚一葉", "KAZUHA MIGIWA"),
               "FRM_0715.eri": ("乃木坂初佳", "MOTOKA NOGISAKA"), "FRM_0716.eri": ("其他", "OTHERS")},
    "haruka": {"FRM_0711.eri": ("春日野穹", "SORA KASUGANO"), "FRM_0717.eri": ("伊福部八寻", "YAHIRO IFUKUBE"),
               "FRM_0718.eri": ("仓永梢", "KOZUE KURANAGA"), "FRM_0716.eri": ("其他", "OTHERS")},
}
TAG = (522, 34)
TAG_FACE = (15, 7, 73, 27)          # the original portrait window inside each tab
PAGE_TABS = {"FRM_0721.eri": "CG1", "FRM_0722.eri": "CG2", "FRM_0723.eri": "CG3", "FRM_0724.eri": "回想",
             "FRM_0725.eri": None}  # None: the person glyph (Haruka's character pages)
# Track titles in the list texture's row order (the page arranges the rows).
MUSIC = ["道の先、空の向こう", "優しい風が吹くあの場所で", "子守歌じゃないのに", "スローライフに憧れて",
         "大空と紙飛行機", "膝枕で過ごす放課後", "素直と不器用と意地っ張り", "黄昏の帰り道",
         "頬を伝う冷たい涙", "漆黒の思惑", "気持ちの狭間", "むふ～", "よーしやっちまえ！",
         "楽しんだ今が勝ち！", "寂しい夜", "無くならない大事な心", "追い求めてきたもの", "星の滴",
         "未来へ踏み出す一歩", "開闢新地", "夜明けのプリズム", "フタリ"]
MUSIC_ROWS = {"yosuga": 21, "haruka": 22}


def gallery_header():
    im = canvas((131, 26))
    halo = (0, 0.8, 1.6, rgba(TEAL_INK, 170))
    text(im, (3, 19.5), "GALLERY", LATIN_ROUND_B, 14, (255, 255, 255), tracking=2.2, shadow=halo)
    w = text_width("GALLERY", LATIN_ROUND_B, 14, 2.2)
    text(im, (w + 10, 19), "鉴赏", ROUND_B, 11.5, (255, 255, 255), shadow=halo)
    hgradient_line(im, 3, 128, 24.2, (255, 255, 255), 170, 0.8, fade=0.02)
    return im


def tag_bar(face, name, roman):
    """face: the original portrait strip at 4x (TAG_FACE size), or None."""
    cells = []
    x0, y0, x1, y1 = TAG_FACE
    for focus in (False, True):
        cell = canvas(TAG)
        coords = (1, 1, 521, 33)
        glow(cell, (4, 4, 518, 34), 11, TEAL_INK, blur=2.0, alpha=36)
        if focus:
            glass(cell, coords, 11, rgba(ACCENT_TOP, 250), rgba(ACCENT_BOT, 252), border=rgba((255, 255, 255), 210),
                  highlight=190)
        else:
            glass(cell, coords, 11, rgba((255, 255, 255), 238), rgba((238, 247, 252), 242), border=rgba(LINE, 180),
                  highlight=255)
        if face is not None:
            mask = rounded_mask(TAG, (x0, y0, x1, y1), 6)
            layer = canvas(TAG)
            layer.paste(face, box((x0, y0)))
            layer.putalpha(ImageChops.multiply(layer.getchannel("A"), mask))
            cell.alpha_composite(layer)
            stroke_round(cell, (x0 - 0.6, y0 - 0.6, x1 + 0.6, y1 + 0.6), 6.4, rgba((255, 255, 255), 230), 1.1)
        ink = (255, 255, 255) if focus else INK
        text(cell, (90, 21.6), name, ROUND_B, 14.5, ink)
        text(cell, (90 + text_width(name, ROUND_B, 14.5) + 9, 21.0), roman, LATIN_ROUND_B, 6.2,
             rgba((255, 255, 255), 230) if focus else rgba(ACCENT, 255), tracking=1.3)
        cells.append(cell)
    return sheet(cells, TAG)


def person_glyph(im, cx, cy, color):
    ellipse(im, (cx - 3.2, cy - 6.6, cx + 3.2, cy - 0.2), fill=color)
    layer = canvas((im.width / S, im.height / S))
    ImageDraw.Draw(layer).pieslice(box((cx - 6.4, cy + 0.6, cx + 6.4, cy + 13.4)), 180, 360, fill=rgba(color))
    im.alpha_composite(layer)


def page_tab(label):
    """41x28 per state: normal, focus, selected."""
    cells = []
    for state in range(3):
        cell = canvas((41, 28))
        coords = (3.5, 3.5, 37.5, 24.5)
        if state == 0:
            fill_round(cell, coords, 9, rgba((255, 255, 255), 250))
            stroke_round(cell, coords, 9, rgba(LINE, 220), 0.8)
            ink = INK_SUB
        elif state == 1:
            fill_round(cell, coords, 9, rgba(ACCENT_SOFT, 255))
            stroke_round(cell, coords, 9, rgba(ACCENT, 255), 0.9)
            ink = INK
        else:
            glow(cell, coords, 9, SKY, blur=1.2, alpha=100)
            glass(cell, coords, 9, rgba(ACCENT_TOP, 255), rgba(ACCENT_BOT, 255), border=rgba((255, 255, 255), 210),
                  highlight=170)
            ink = (255, 255, 255)
        if label is None:
            person_glyph(cell, 20.5, 14.2, ink)
        elif label.isascii():
            text(cell, (20.5, 17.6), label, LATIN_ROUND_B, 9.6, ink, anchor="ms", tracking=0.4)
        else:
            text(cell, (20.5, 18.0), label, ROUND_B, 10.5, ink, anchor="ms")
        cells.append(cell)
    return sheet(cells, (41, 28))


def gallery_panel(size):
    im = canvas(size)
    w, h = size
    glow(im, (4, 6, w - 4, h - 2), 14, TEAL_INK, blur=3.4, alpha=56)
    glass(im, (1.5, 1.5, w - 1.5, h - 1.5), 14, rgba((255, 255, 255), 236), rgba((236, 246, 251), 242),
          border=rgba(LINE, 190), highlight=255)
    return im


def thumb_frame(cell_size):
    """Two states: a quiet hairline at rest, a lit sky frame on focus."""
    w, h = cell_size
    cells = []
    for focus in (False, True):
        cell = canvas(cell_size)
        if focus:
            glow(cell, (1, 1, w - 1, h - 1), 6, SKY, blur=1.6, alpha=150)
            stroke_round(cell, (0.8, 0.8, w - 0.8, h - 0.8), 6, rgba(ACCENT, 255), 2.2)
        else:
            stroke_round(cell, (0.6, 0.6, w - 0.6, h - 0.6), 6, rgba((255, 255, 255), 210), 1.0)
        cells.append(cell)
    return sheet(cells, cell_size)


def lock_glyph(im, cx, cy, color):
    layer = canvas((im.width / S, im.height / S))
    ImageDraw.Draw(layer).arc(box((cx - 3.4, cy - 7.4, cx + 3.4, cy - 0.6)), 180, 360, fill=rgba(color),
                              width=round(1.2 * S))
    im.alpha_composite(layer)
    polyline(im, [(cx - 3.4, cy - 4), (cx - 3.4, cy - 1.4)], color, 1.2)
    polyline(im, [(cx + 3.4, cy - 4), (cx + 3.4, cy - 1.4)], color, 1.2)
    fill_round(im, (cx - 5.2, cy - 1.4, cx + 5.2, cy + 6.4), 1.6, rgba(color))


def empty_slot(size):
    im = canvas(size)
    w, h = size
    fill_round(im, (1, 1, w - 1, h - 1), 6, rgba((222, 239, 248), 255))
    stroke_round(im, (1, 1, w - 1, h - 1), 6, rgba(LINE_SOFT, 255), 0.8)
    lock_glyph(im, w / 2, h / 2 - 3, INK_FAINT)
    text(im, (w / 2, h / 2 + 15), "LOCKED", LATIN_ROUND_B, 6.4, INK_FAINT, anchor="ms", tracking=1.4)
    return im


def music_list(rows):
    cells = []
    for focus in (False, True):
        col = canvas((255, 22 * rows))
        for i in range(rows):
            y = i * 22
            if focus:
                fill_round(col, (6, y + 1.5, 249, y + 20.5), 9.5, rgba(ACCENT_SOFT, 255))
                stroke_round(col, (6, y + 1.5, 249, y + 20.5), 9.5, rgba(ACCENT, 255), 0.8)
            note = ACCENT if focus else rgba(LINE, 255)
            text(col, (17, y + 15.6), "♪", ROUND_B, 9.5, note, anchor="ms")
            text(col, (131, y + 15.4), MUSIC[i], MARU, 11.6, INK if focus else INK_SUB, anchor="ms")
        cells.append(col)
    return sheet(cells, (255, 22 * rows))


def stop_button():
    cells = []
    for focus in (False, True):
        cell = canvas((131, 26))
        coords = (3, 2.5, 128, 23.5)
        if focus:
            glow(cell, coords, 10.5, SKY, blur=1.4, alpha=100)
            glass(cell, coords, 10.5, rgba(ACCENT_TOP, 255), rgba(ACCENT_BOT, 255), border=rgba((255, 255, 255), 200),
                  highlight=170)
            ink = (255, 255, 255)
        else:
            fill_round(cell, coords, 10.5, rgba((255, 255, 255), 255))
            stroke_round(cell, coords, 10.5, rgba(LINE, 220), 0.8)
            ink = INK
        lw = text_width("停止播放", ROUND_B, 12)
        x = 65.5 - (lw + 14) / 2
        fill_round(cell, (x, 9.2, x + 7.6, 16.8), 1.4, rgba(ink))
        text(cell, (x + 14, 17.4), "停止播放", ROUND_B, 12, ink)
        cells.append(cell)
    return sheet(cells, (131, 26))


def viewer_track(size):
    im = canvas(size)
    w, h = size
    fill_round(im, (1, 6, w - 1, h - 6), 3, rgba((255, 255, 255), 170))
    stroke_round(im, (1, 6, w - 1, h - 6), 3, rgba(TEAL_INK, 70), 0.6)
    return im


def viewer_knob():
    cells = []
    for state in range(3):
        cell = canvas((18, 18))
        ellipse(cell, (2.4, 3.2, 15.6, 16.4), fill=rgba(TEAL_INK, 70))
        ellipse(cell, (2.4, 2.2, 15.6, 15.4), fill=(255, 255, 255, 255), outline=rgba(ACCENT, 255), width=1.2)
        if state:
            ellipse(cell, (6, 5.8, 12, 11.8), fill=rgba(ACCENT, 200 if state == 1 else 255))
        cells.append(cell)
    return sheet(cells, (18, 18))


def viewer_close():
    cells = []
    for focus in (False, True):
        cell = canvas((16, 17))
        ellipse(cell, (0.4, 0.9, 15.6, 16.1), fill=rgba(ACCENT if focus else (255, 255, 255), 240))
        col = (255, 255, 255) if focus else INK
        polyline(cell, [(5.0, 5.5), (11.0, 11.5)], col, 1.2)
        polyline(cell, [(11.0, 5.5), (5.0, 11.5)], col, 1.2)
        cells.append(cell)
    return sheet(cells, (16, 17))


def option_masters(game, sizes, faces):
    """sizes: archive name -> original logical size; faces: tag name -> 4x portrait strip."""
    m = {"FRM_0702.eri": gallery_header(), "FRM_0731.eri": gallery_panel(sizes["FRM_0731.eri"]),
         "FRM_0741.eri": gallery_panel(sizes["FRM_0741.eri"]),
         "FRM_0732.eri": thumb_frame((120, 90)), "FRM_0734.eri": thumb_frame((242, 90)),
         "FRM_0733.eri": empty_slot(sizes["FRM_0733.eri"]), "FRM_0735.eri": empty_slot(sizes["FRM_0735.eri"]),
         "FRM_0742.eri": music_list(MUSIC_ROWS[game]), "FRM_0743.eri": stop_button(),
         "FRM_0751.eri": viewer_track(sizes["FRM_0751.eri"]), "FRM_0752.eri": viewer_knob(),
         "FRM_0119.eri": viewer_close()}
    for name, (label, roman) in OPTION_TAGS[game].items():
        m[name] = tag_bar(faces.get(name), label, roman)
    for name, label in PAGE_TABS.items():
        if name in sizes:
            m[name] = page_tab(label)
    return m


# --------------------------------------------------------------------------
# the ハルカナソラ entry on Yosuga's title (made like make_haruka_entry.py, at 4x)

HARUKA_ENTRY = "haruka_entry.png"
HARUKA_ENTRY_MASK = "haruka_entry_mask.png"


def haruka_entry_master(logo4x, scale=0.6, pad=8):
    """logo4x: the corrected 4x Haruka logo (1416x300). Returns (master sheet, logical cell size)."""
    w = round(354 * scale)
    h = round(75 * w / 354)
    cw, ch = w + pad * 2, h + pad * 2
    small = logo4x.convert("RGBA").resize((w * S, h * S), Image.Resampling.LANCZOS)
    normal = canvas((cw, ch))
    rest = small.copy()
    rest.putalpha(rest.getchannel("A").point(lambda a: int(a * 0.82)))
    normal.alpha_composite(rest, (pad * S, pad * S))
    focus = canvas((cw, ch))
    mask = Image.new("L", focus.size, 0)
    mask.paste(small.getchannel("A"), (pad * S, pad * S))
    halo = Image.new("RGBA", focus.size, (255, 255, 255, 0))
    halo.putalpha(mask.filter(ImageFilter.GaussianBlur(5 * S)).point(lambda a: min(255, int(a * 0.9))))
    focus.alpha_composite(halo)
    from PIL import ImageEnhance
    bright = ImageEnhance.Brightness(small.convert("RGB")).enhance(1.12).convert("RGBA")
    bright.putalpha(small.getchannel("A"))
    focus.alpha_composite(bright, (pad * S, pad * S))
    return sheet([normal, focus], (cw, ch)), (cw, ch)
