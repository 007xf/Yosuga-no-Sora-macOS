#!/usr/bin/env python3
"""Build every source/frame replacement from the immutable archive census.

Requires tools/hd/bulk/catalog_export.py to complete first. A verified runtime
manifest is published only when *every* catalog frame is covered and passes.
The native renderer resolves encoded member SHA-256 and original frame number;
original archive bytes, frame order, dimensions, hotspot and masks stay intact.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import importlib.util
import json
import os
from pathlib import Path
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location("hd_bulk_upscale", Path(__file__).with_name("bulk_upscale.py"))
BULK = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(BULK)


def make_tasks(catalog, sources: Path, output: Path, scale: int):
    unique = {}
    for asset in catalog["assets"]:
        digest = asset["encoded_sha256"]
        if digest not in unique:
            unique[digest] = {"asset": asset, "aliases": []}
        unique[digest]["aliases"].append(asset["id"])
    tasks, errors = [], []
    for digest, item in unique.items():
        asset = item["asset"]
        metadata_path = sources / digest / "metadata.json"
        try:
            metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
            for field in ("width", "height", "frames", "format", "alpha_mode", "origin", "duration_ms", "sequence"):
                if metadata.get(field, [] if field == "sequence" else None) != asset.get(field, [] if field == "sequence" else None):
                    raise ValueError(f"Decoded source {field} differs from original census")
            if metadata.get("verified_source_metadata_sha256") != asset["metadata_sha256"]:
                raise ValueError("Decoded export was not verified against the original ERI metadata")
            if len(metadata.get("pngs", [])) != asset["frames"]:
                raise ValueError("Decoded source PNG frame inventory is incomplete")
            frames = {p["frame"]: p for p in metadata["pngs"]}
            for frame in range(asset["frames"]):
                source = sources / digest / f"frame{frame:04d}.png"
                if frame not in frames:
                    raise ValueError(f"Missing original frame {frame}")
                record = {
                    "id": f"{digest}:{frame}", "aliases": item["aliases"],
                    "encoded_sha256": digest, "source_sha256": asset["source_sha256"],
                    "frame": frame, "original_frames": asset["frames"],
                    "game": asset["game"], "category": asset["category"],
                    "source": str(source), "output": str(output / digest / source.name),
                    "source_size": asset["source_size"], "scale": scale,
                    "alpha_mode": asset["alpha_mode"], "format": asset["format"],
                    "origin": asset["origin"], "duration_ms": asset["duration_ms"], "sequence": asset.get("sequence", []),
                    "expected_decoded_png_sha256": frames[frame]["sha256"],
                }
                tasks.append(record)
        except Exception as error:
            errors.append({"encoded_sha256": digest, "aliases": item["aliases"], "result": "ERROR", "problems": [str(error)]})
    return tasks, errors, unique


def convert(task):
    asset, root, audit, force = task
    try:
        actual = BULK.sha256_file(Path(asset["source"]))
        if actual != asset["expected_decoded_png_sha256"]:
            raise ValueError("Decoded original PNG hash changed since verified export")
        return BULK.process_asset(asset, root=root, audit_root=audit, force=force)
    except Exception as error:
        return {**asset, "result": "ERROR", "problems": [str(error)], "cached": False}


def publish_manifest(path: Path, reports, output: Path):
    output = output.resolve()
    lines = []
    for report in reports:
        if report["result"] != "PASS":
            continue
        relative = Path(report["output"]).resolve().relative_to(output).as_posix()
        row = [report["source_sha256"], report["frame"], *report["source_size"], relative]
        if any("\t" in str(value) or "\n" in str(value) for value in row):
            raise ValueError("Invalid manifest field")
        lines.append("\t".join(str(value) for value in row))
    temporary = path.with_name(path.name + f".{os.getpid()}.part")
    temporary.write_text("\n".join(lines) + "\n", encoding="utf-8")
    os.replace(temporary, path)
    return len(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--catalog", type=Path, default=ROOT / "hd/catalog/01 原始素材清单.json")
    parser.add_argument("--supplemental-catalog", type=Path, action="append", default=[],
                        help="active mod PNG census with the same decoded metadata schema; repeatable")
    parser.add_argument("--sources", type=Path, default=ROOT / "hd/sources")
    parser.add_argument("--output", type=Path, default=ROOT / "hd/bulk")
    parser.add_argument("--workers", type=int, default=2)
    parser.add_argument("--scale", type=int, default=4)
    parser.add_argument("--force", action="store_true")
    parser.add_argument("--plan-only", action="store_true")
    args = parser.parse_args()
    if not 1 <= args.workers <= 4:
        parser.error("workers must be 1..4; default 2 bounds the float32 image memory")
    if not 2 <= args.scale <= 8:
        parser.error("scale must be 2..8")
    sources, output = args.sources.resolve(), args.output.resolve()
    if sources == output:
        parser.error("source and output directories must differ")
    output.mkdir(parents=True, exist_ok=True)
    catalog = json.loads(args.catalog.read_text(encoding="utf-8"))
    original_assets = len(catalog["assets"])
    supplements = []
    for path in args.supplemental_catalog:
        supplemental = json.loads(path.read_text(encoding="utf-8"))
        catalog["assets"].extend(supplemental["assets"])
        supplements.append({"catalog": str(path.resolve()), "sha256": BULK.sha256_file(path),
                            "assets": len(supplemental["assets"])})
    tasks, errors, unique = make_tasks(catalog, sources, output, args.scale)
    expected_unique_frames = sum(item["asset"]["frames"] for item in unique.values())
    expected_asset_frames = sum(a["frames"] for a in catalog["assets"])
    meta = {"method": BULK.VERSION, "catalog": str(args.catalog.resolve()),
            "catalog_sha256": BULK.sha256_file(args.catalog), "scale": args.scale,
            "original_assets": original_assets, "all_input_assets": len(catalog["assets"]),
            "supplemental_catalogs": supplements, "original_asset_frames": expected_asset_frames,
            "unique_sources": len(unique), "expected_unique_frames": expected_unique_frames,
            "prepared_unique_frame_tasks": len(tasks), "source_errors": errors,
            "source_policy": "immutable original archive census; raw decoder export checked by hash and ERI metadata",
            "new_detail_claimed": False}
    BULK.atomic_json(output / "01 高清处理计划.json", meta)
    print(json.dumps({**{k: v for k, v in meta.items() if k != "source_errors"},
                      "source_error_count": len(errors), "source_errors_first_five": errors[:5]}, ensure_ascii=False), flush=True)
    if args.plan_only:
        return bool(errors)
    audit = output / "audit"
    start = time.monotonic()
    counts = {"PASS": 0, "FAIL": 0, "ERROR": len(errors), "cached": 0}
    reports, failures = [], list(errors)
    progress_path = output / "02 处理进度.json"
    log_path = output / "03 处理日志.jsonl"
    def progress(done):
        state = {"done": done, "total": expected_unique_frames, "counts": counts,
                 "elapsed_seconds": round(time.monotonic() - start, 1),
                 "failures": failures, "complete": False}
        BULK.atomic_json(progress_path, state)
        print(json.dumps({k: state[k] for k in ("done", "total", "counts", "elapsed_seconds")}, ensure_ascii=False), flush=True)
    progress(0)
    with log_path.open("a", encoding="utf-8") as log:
        log.write(json.dumps({"event": "run_start", **meta}, ensure_ascii=False) + "\n")
        worker_tasks = ((asset, ROOT, audit, args.force) for asset in tasks)
        with concurrent.futures.ProcessPoolExecutor(max_workers=args.workers) as pool:
            for done, report in enumerate(pool.map(convert, worker_tasks, chunksize=1), 1):
                reports.append(report)
                counts[report["result"]] += 1
                counts["cached"] += bool(report.get("cached"))
                if report["result"] != "PASS":
                    failures.append({"id": report["id"], "aliases": report.get("aliases"),
                                     "result": report["result"], "problems": report["problems"]})
                log.write(json.dumps({"id": report["id"], "result": report["result"], "cached": report.get("cached"),
                                      "audit": report.get("audit"), "problems": report["problems"]}, ensure_ascii=False) + "\n")
                if done % 10 == 0 or report["result"] != "PASS" or done == len(tasks):
                    log.flush()
                    progress(done)
    complete = counts["PASS"] == expected_unique_frames and counts["FAIL"] == counts["ERROR"] == 0
    candidate = output / "manifest.candidate.tsv"
    manifest_rows = publish_manifest(candidate, reports, output)
    manifest = output / "manifest.tsv"
    if complete:
        os.replace(candidate, manifest)
    summary = {**meta, "result": "PASS" if complete else "FAIL", "complete": complete,
               "counts": counts, "manifest_rows": manifest_rows,
               "manifest": str(manifest if complete else candidate),
               "manifest_sha256": BULK.sha256_file(manifest if complete else candidate),
               "elapsed_seconds": round(time.monotonic() - start, 2), "failures": failures,
               "assets": reports}
    BULK.atomic_json(output / "04 高清处理汇总.json", summary)
    BULK.atomic_json(progress_path, {k: v for k, v in summary.items() if k != "assets"})
    print(json.dumps({k: v for k, v in summary.items() if k not in ("assets", "source_errors", "failures")}, ensure_ascii=False), flush=True)
    return 0 if complete else 1


if __name__ == "__main__":
    sys.exit(main())
