#!/usr/bin/env python3
"""Offline previews of the 空澄 interface over real backgrounds (no game run).

Composes the 4x masters from mods/src/sky_ui.py with stand-in text drawn the
way the engine draws it (Hiragino Sans GB at the skin's font sizes), so the
layout and contrast can be judged quickly. The real check is always the game.

    python3 tools/ui_mockup.py <background.png> <out.png> [scene]
scene: adv (default) | choice
"""
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "mods/src"))
import sky_ui as U  # noqa: E402

SAMPLE_NAME = "春日野穹"
SAMPLE_LINES = ["这是用于检查排版的示例文字，与正文同样的字号和行距，",
                "用来确认三行文字、名字栏与下方菜单之间的留白是否足够，",
                "以及在明亮和昏暗的背景上是否都清楚易读。"]


def place(screen, master, xy):
    screen.alpha_composite(master, (round(xy[0] * U.S), round(xy[1] * U.S)))


def crop_cell(sheet, index, size):
    w, h = round(size[0] * U.S), round(size[1] * U.S)
    return sheet.crop((index * w, 0, index * w + w, h))


def adv(screen, game="yosuga"):
    m = U.frame_masters(game)
    ox, oy = 25, 444
    place(screen, m["FRM_0102b.eri"], (ox, oy))
    U.text(screen, (ox + 27, oy + 5 + 22), SAMPLE_NAME, U.CJK_W6, 22, U.WHITE,
           shadow=(0, 1, 0, U.rgba(U.TEAL_INK, 255)))
    for i, line in enumerate(SAMPLE_LINES):
        U.text(screen, (ox + 39, oy + 40 + 20 + i * 26), line, U.CJK_W3, 20, U.WHITE,
               shadow=(0, 1, 0, U.rgba(U.TEAL_INK, 255)))
    for i, (name, label, kind, toggles) in enumerate(U.MENU_ITEMS):
        x = [0, 92, 184, 276, 368, 460, 552, 644][i]
        cell = crop_cell(m[name], 1 if i == 2 else 0, U.MENU_CELL)
        place(screen, cell, (ox + 9 + x, oy + 124))
    place(screen, crop_cell(m["FRM_0119.eri"], 0, (16, 17)), (ox + 727, oy + 7))
    place(screen, crop_cell(m["FRM_0105.eri"], 0, (28, 21)), (ox + 689, oy + 6))
    place(screen, m["FRM_0101.eri"], (ox, oy))


def choice(screen, game="yosuga"):
    m = U.frame_masters(game)
    for i, label in enumerate(["示例选项一", "示例选项二"]):
        y = 200 + i * 64
        place(screen, crop_cell(m["FRM_0104.eri"], i, U.CHOICE), (25, y))
        U.text(screen, (25 + 48, y + 13 + 19), label, U.CJK_W6, 21, U.INK)


def main():
    bg, out = Path(sys.argv[1]), Path(sys.argv[2])
    scene = sys.argv[3] if len(sys.argv) > 3 else "adv"
    screen = Image.open(bg).convert("RGBA").resize((800 * U.S, 600 * U.S), Image.Resampling.LANCZOS)
    {"adv": adv, "choice": choice}[scene](screen)
    screen.convert("RGB").resize((1600, 1200), Image.Resampling.LANCZOS).save(out)


if __name__ == "__main__":
    main()
