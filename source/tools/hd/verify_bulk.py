#!/usr/bin/env python3
"""Read back every local or packaged HD replacement against its audit and census."""
import argparse
import importlib.util
import json
from pathlib import Path
import struct
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location("hd_bulk_upscale", Path(__file__).with_name("bulk_upscale.py"))
BULK = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(BULK)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("hd_root", type=Path)
    parser.add_argument("--summary", type=Path, default=ROOT / "hd/bulk/04 高清处理汇总.json")
    parser.add_argument("--catalog", type=Path, default=ROOT / "hd/catalog/01 原始素材清单.json")
    parser.add_argument("--supplemental-catalog", type=Path, action="append", default=[])
    parser.add_argument("--check-sources", action="store_true", help="also rehash the immutable decoded original PNGs")
    parser.add_argument("--report", type=Path, default=ROOT / "hd/catalog/05 高清回读核验.json")
    args = parser.parse_args()
    start = time.monotonic()
    output = args.hd_root.resolve()
    summary = json.loads(args.summary.read_text(encoding="utf-8"))
    if summary.get("result") != "PASS" or not summary.get("complete"):
        raise ValueError("The build summary does not prove complete acceptance")
    assets = []
    for catalog in [args.catalog, *args.supplemental_catalog]:
        assets.extend(json.loads(catalog.read_text(encoding="utf-8"))["assets"])
    expected = {(a["source_sha256"], frame): tuple(a["source_size"])
                for a in assets for frame in range(a["frames"])}
    manifest = output / "manifest.tsv"
    rows = {}
    errors = []
    for number, line in enumerate(manifest.read_text(encoding="utf-8").splitlines(), 1):
        try:
            digest, frame, width, height, relative = line.split("\t")
            key = (digest, int(frame))
            if key in rows:
                raise ValueError("duplicate source/frame key")
            if key not in expected or expected[key] != (int(width), int(height)):
                raise ValueError("source key or logical dimensions absent from original census")
            path = (output / relative).resolve()
            if not path.is_relative_to(output):
                raise ValueError("PNG path leaves HD root")
            rows[key] = {"path": path, "relative": relative, "size": (int(width), int(height))}
        except Exception as error:
            errors.append({"manifest_line": number, "error": str(error)})
    missing = sorted(set(expected) - set(rows))
    if missing:
        errors.append({"missing_original_frames": len(missing), "first_five": missing[:5]})
    accepted = {(r["source_sha256"], r["frame"]): r for r in summary["assets"]}
    total_bytes = 0
    verified = 0
    for key, row in rows.items():
        try:
            record = accepted[key]
            if record["result"] != "PASS" or not record["source_original_unchanged"]:
                raise ValueError("build audit does not prove strict acceptance and immutable source")
            path = row["path"]
            wanted = tuple(v * summary["scale"] for v in row["size"])
            if not BULK.png_rgba8(path, wanted):
                raise ValueError("PNG is not exact-scale RGBA8")
            if BULK.sha256_file(path) != record["output_sha256"]:
                raise ValueError("replacement bytes differ from losslessly read-back audited result")
            if args.check_sources and BULK.sha256_file(Path(record["source"])) != record["source_decoded_png_sha256"]:
                raise ValueError("decoded original PNG changed after conversion")
            if (record["alpha_iou"] < BULK.LIMITS["alpha_iou_min"] or
                    record["colour_diff"] > BULK.LIMITS["colour_diff_max"] or record["ssim"] < BULK.LIMITS["ssim_min"]):
                raise ValueError("strict numerical acceptance thresholds not satisfied")
            verified += 1
            total_bytes += path.stat().st_size
            if verified % 500 == 0:
                print(json.dumps({"verified": verified, "total": len(expected), "errors": len(errors)}, ensure_ascii=False), flush=True)
        except Exception as error:
            errors.append({"source_sha256": key[0], "frame": key[1], "error": str(error)})
    if BULK.sha256_file(manifest) != summary["manifest_sha256"]:
        errors.append({"error": "runtime manifest bytes differ from accepted build manifest"})
    report = {"result": "PASS" if verified == len(expected) and not errors else "FAIL",
              "hd_root": str(output), "summary": str(args.summary.resolve()), "expected_unique_frames": len(expected),
              "verified_replacement_frames": verified, "png_bytes": total_bytes, "scale": summary["scale"],
              "source_readback": args.check_sources, "manifest_sha256": BULK.sha256_file(manifest),
              "elapsed_seconds": round(time.monotonic() - start, 2), "errors": errors}
    BULK.atomic_json(args.report, report)
    print(json.dumps(report, ensure_ascii=False), flush=True)
    return report["result"] != "PASS"


if __name__ == "__main__":
    sys.exit(main())
