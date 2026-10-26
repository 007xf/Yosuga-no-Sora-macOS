#!/usr/bin/env python3
"""Deduplicate a metadata census and decode only its image entries with native ERI SDK."""
from concurrent.futures import ThreadPoolExecutor
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--catalog", type=Path, default=ROOT / "hd/catalog/01 原始素材清单.json")
    parser.add_argument("--output", type=Path, default=ROOT / "hd/sources")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--plan-only", action="store_true")
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--asset", action="append", default=[], help="optional full catalog asset ID; repeatable")
    parser.add_argument("--verify", action="store_true", help="verify completed exports and write decode summary")
    args = parser.parse_args()
    catalog = json.loads(args.catalog.read_text(encoding="utf-8"))
    selected = [a for a in catalog["assets"] if not args.asset or a["id"] in args.asset]
    unique = {}
    for asset in selected:
        unique.setdefault(asset["encoded_sha256"], asset)
    rows = list(unique.values())
    if args.limit:
        rows = rows[:args.limit]
    args.output.mkdir(parents=True, exist_ok=True)
    plan_dir = ROOT / "work/hd/export"
    plan_dir.mkdir(parents=True, exist_ok=True)
    jobs = max(1, min(args.jobs, len(rows))) if rows else 1
    plans = []
    for worker in range(jobs):
        plan = plan_dir / f"plan-{worker:02d}.tsv"
        lines = []
        for asset in rows[worker::jobs]:
            values = [asset["encoded_sha256"], asset["archive"], asset["payload_offset"],
                      asset["stored_bytes"], asset["source_bytes"], asset["encoding"], asset["id"]]
            if any("\t" in str(v) or "\n" in str(v) for v in values):
                raise ValueError("unsafe TSV field")
            lines.append("\t".join(str(v) for v in values))
        plan.write_text("\n".join(lines) + "\n", encoding="utf-8")
        plans.append(plan)
    print(json.dumps({"unique_sources": len(rows), "frames": sum(a["frames"] for a in rows),
                      "plans": [str(p) for p in plans], "output": str(args.output)}, ensure_ascii=False), flush=True)
    if args.plan_only:
        return 0
    if not args.verify:
        env = dict(os.environ, ERI_EXPORT_PNG_LEVEL="1")
        def run(plan):
            log = plan.with_suffix(".log")
            with log.open("w", encoding="utf-8") as output:
                result = subprocess.run([str(ROOT / "tools/eri_export/build/eri_export"), "batch", str(plan),
                                         str(args.output)], stdout=output, stderr=subprocess.STDOUT, env=env)
            return {"plan": str(plan), "log": str(log), "exit_code": result.returncode}
        with ThreadPoolExecutor(max_workers=jobs) as pool:
            results = list(pool.map(run, plans))
        for result in results:
            print(json.dumps(result), flush=True)
        if any(r["exit_code"] for r in results):
            return 1
    errors = []
    verified_frames = 0
    for asset in rows:
        directory = args.output / asset["encoded_sha256"]
        metadata = directory / "metadata.json"
        try:
            meta = json.loads(metadata.read_text(encoding="utf-8"))
            for field in ("width", "height", "frames", "format", "alpha_mode", "origin", "duration_ms", "sequence"):
                expected = asset.get(field, [] if field == "sequence" else None)
                if meta[field] != expected:
                    raise ValueError(f"{field}: {meta[field]!r} != {expected!r}")
            # Check PNG signature, dimensions and pixel mode without displaying any artwork.
            from PIL import Image
            pngs = []
            for frame in range(asset["frames"]):
                image_path = directory / f"frame{frame:04d}.png"
                with Image.open(image_path) as image:
                    image.load()
                    if image.size != (asset["width"], asset["height"]) or image.mode != "RGBA":
                        raise ValueError(f"invalid PNG properties: frame {frame}")
                pngs.append({"frame": frame, "file": image_path.name, "bytes": image_path.stat().st_size,
                             "sha256": hashlib.sha256(image_path.read_bytes()).hexdigest()})
                verified_frames += 1
            meta["pngs"] = pngs
            meta["verified_source_metadata_sha256"] = asset["metadata_sha256"]
            metadata.write_text(json.dumps(meta, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
        except Exception as exc:
            errors.append({"id": asset["id"], "error": str(exc)})
    report = {"catalog": str(args.catalog), "unique_sources": len(rows), "verified_frames": verified_frames,
              "errors": errors, "result": "PASS" if not errors else "FAIL"}
    report_path = plan_dir / "decode-summary.json"
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False), flush=True)
    return bool(errors)


if __name__ == "__main__":
    sys.exit(main())
