#!/usr/bin/env python3
"""Prepare isolated real-image fixtures and run headless native HD binding tests."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("hd_inventory", Path(__file__).with_name("inventory.py"))
INVENTORY = importlib.util.module_from_spec(spec)
spec.loader.exec_module(INVENTORY)


def prepare(catalog_path, supplemental_path, directory, output):
    catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
    assets = catalog["assets"]
    selected = [next(a for a in assets if a["id"] == "yosuga/system/title.noa/FRM_0612.eri"),
                next(a for a in assets if a["game"] == "yosuga" and a["entry"].lower() == "b27a.eri"),
                next(a for a in assets if a["frames"] == 27),
                next(a for a in assets if a["id"] == "yosuga/system/frame.noa/FRM_0103.eri"),
                next(a for a in assets if a.get("grayscale") and a["width"] >= 8 and a["height"] >= 8)]
    if supplemental_path.is_file():
        supplemental = json.loads(supplemental_path.read_text(encoding="utf-8"))
        selected.append(next(a for a in supplemental["assets"] if a["id"] == "yosuga/mods/title.noa/haruka_entry.png"))
    directory.mkdir(parents=True, exist_ok=True)
    decoder = INVENTORY.load_erisan()
    lines, report = [], []
    for number, asset in enumerate(selected):
        archive = Path(asset["archive"])
        with archive.open("rb") as source:
            entry = next(a for a in INVENTORY.noa_directory(source) if a["entry"] == asset["entry"])
            source.seek(entry["payload_offset"])
            encoded = source.read(entry["stored_bytes"])
        if hashlib.sha256(encoded).hexdigest() != asset["encoded_sha256"]:
            raise ValueError(f"Source archive payload changed: {asset['id']}")
        decoded = encoded if entry["encoding"] == 0 else decoder(encoded).decode(entry["size"])
        if hashlib.sha256(decoded).hexdigest() != asset["source_sha256"]:
            raise ValueError(f"Source decoded stream fingerprint changed: {asset['id']}")
        target = directory / f"source-{number:04d}{Path(asset['entry']).suffix}"
        target.write_bytes(decoded)
        raw_root = ROOT / "hd/sources" / asset["encoded_sha256"]
        values = [asset["id"], asset["source_sha256"], asset["encoded_sha256"], target,
                  raw_root, output / asset["encoded_sha256"], asset["width"], asset["height"], asset["frames"],
                  asset["format"], asset["depth"], *asset["origin"]]
        if any("\t" in str(v) or "\n" in str(v) for v in values):
            raise ValueError("Unsafe TSV fixture field")
        lines.append("\t".join(str(v) for v in values))
        report.append({"id": asset["id"], "source_sha256": asset["source_sha256"],
                       "encoded_sha256": asset["encoded_sha256"], "source_copy": str(target),
                       "source_copy_sha256": hashlib.sha256(target.read_bytes()).hexdigest(), "frames": asset["frames"]})
    table = directory / "fixtures.tsv"
    table.write_text("\n".join(lines) + "\n", encoding="utf-8")
    (directory / "01 原始素材身份验证.json").write_text(json.dumps({"result": "PASS", "fixtures": report}, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
    return table, report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--catalog", type=Path, default=ROOT / "hd/catalog/01 原始素材清单.json")
    parser.add_argument("--supplemental", type=Path, default=ROOT / "hd/catalog/03 当前覆盖PNG素材.json")
    parser.add_argument("--fixtures", type=Path, default=ROOT / "work/hd/runtime")
    parser.add_argument("--hd-root", type=Path, default=ROOT / "hd/bulk")
    parser.add_argument("--test-binary", type=Path,
                        default=ROOT / "vendor/EntisGLS-SDL3/build/macos-sdl3-arm64/launcher_hd_images_test")
    parser.add_argument("--prepare-only", action="store_true")
    args = parser.parse_args()
    table, report = prepare(args.catalog, args.supplemental, args.fixtures, args.hd_root)
    print(json.dumps({"fixtures": len(report), "fixture_frames": sum(a["frames"] for a in report), "table": str(table),
                      "source_fingerprints": "PASS"}, ensure_ascii=False), flush=True)
    if args.prepare_only:
        return 0
    if not (args.hd_root / "manifest.tsv").is_file():
        raise ValueError("Complete, verified HD manifest is not published yet")
    log = args.fixtures / "02 原生高清绑定测试.log"
    with log.open("w", encoding="utf-8") as output:
        result = subprocess.run([str(args.test_binary), str(args.fixtures), str(table), str(args.hd_root)],
                                stdout=output, stderr=subprocess.STDOUT)
    print(log.read_text(encoding="utf-8"), end="")
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
