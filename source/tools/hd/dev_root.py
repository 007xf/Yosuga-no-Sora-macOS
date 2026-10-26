#!/usr/bin/env python3
"""Assemble a development HD root (work/hd-dev) for previewing UI changes.

It links the accepted hd/bulk set and adds the 4x masters that the 空澄 skin
builder wrote for the current mods (mods/build/work/{frame,title}/*/masters),
so the dev build shows the new interface at full resolution:

    ENTISGLS_HD_ROOT="$PWD/work/hd-dev" <app>/Contents/MacOS/Yosuga

Development only; packaging uses the packed lossless store (pack_store.py).
"""
import json
import os
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "work/hd-dev"


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    link = OUT / "bulk"
    if not link.exists():
        os.symlink(ROOT / "hd/bulk", link)
    rows = []
    for line in (ROOT / "hd/bulk/manifest.tsv").read_text(encoding="utf-8").splitlines():
        if line and not line.startswith("#"):
            key, frame, w, h, path = line.split("\t")
            rows.append("\t".join((key, frame, w, h, "bulk/" + path)))
    sky = OUT / "sky"
    sky.mkdir(exist_ok=True)
    added = 0
    for report in sorted((ROOT / "mods/build/work").glob("*/*/validation.json")):
        textures = json.loads(report.read_text(encoding="utf-8"))["textures"]
        for item in textures.values():
            master = report.parent / "masters" / (item["sha256"] + ".png")
            w, h = item["size"]
            with Image.open(master) as im:
                if im.size != (w * 4, h * 4):
                    raise ValueError(f"{master}: not 4x {w}x{h}")
            target = sky / master.name
            if not target.exists():
                os.link(master, target)
            rows.append("\t".join((item["sha256"], "0", str(w), str(h), "sky/" + master.name)))
            added += 1
    (OUT / "manifest.tsv").write_text("\n".join(rows) + "\n", encoding="utf-8")
    print(json.dumps({"root": str(OUT), "rows": len(rows), "sky_masters": added}))


if __name__ == "__main__":
    main()
