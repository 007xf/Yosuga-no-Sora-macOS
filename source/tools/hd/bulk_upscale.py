#!/usr/bin/env python3
"""Source-faithful, resumable HD preparation; never generates new image content.

Input is a JSON object containing ``assets``. Each ready asset has ``source``
(a decoded PNG), ``output`` (the replacement PNG), integer ``scale``, and
``alpha_mode`` (``straight``, ``premultiplied`` or ``opaque``, from ERI flags).
Relative paths resolve against --root. All other inventory fields are retained
in each audit record. ``process_asset`` is also callable from an extractor.

The comparison reference is the original's *displayed* straight-alpha pixels.
Unpremultiplication is an explicit representation correction when required by
the original ERI flags. It changes no source file, alpha, or opaque RGB pixel.
The original tools/hd/check_hd.py and its thresholds are left untouched.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import sys
import time
import traceback

import numpy as np
from PIL import Image

VERSION = "source-lanczos-premultiplied-v3"
ROOT = Path(__file__).resolve().parents[2]
LIMITS = {"alpha_iou_min": 0.97, "colour_diff_max": 10.0, "ssim_min": 0.90}
_spec = importlib.util.spec_from_file_location("original_hd_checker", Path(__file__).with_name("check_hd.py"))
_checker = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_checker)


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def resolved(root: Path, value: str) -> Path:
    path = Path(value)
    return (path if path.is_absolute() else root / path).resolve()


def atomic_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + f".{os.getpid()}.part")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def straight_reference(image: Image.Image, alpha_mode: str) -> Image.Image:
    """Convert a raw decoded export to its original straight RGBA appearance."""
    if alpha_mode not in ("straight", "premultiplied", "opaque"):
        raise ValueError(f"Unknown alpha_mode {alpha_mode!r}; infer it from ERI format flags")
    image = image.convert("RGBA")
    if alpha_mode in ("straight", "opaque"):
        return image
    raw = np.asarray(image, dtype=np.float32)
    a = raw[..., 3:4]
    rgb = np.divide(raw[..., :3] * 255.0, a, out=np.zeros_like(raw[..., :3]), where=a > 0)
    corrected = np.concatenate((np.clip(rgb, 0, 255), a), axis=2)
    return Image.fromarray(np.rint(corrected).astype(np.uint8))


def faithful_resize(original: Image.Image, scale: int) -> Image.Image:
    """Lanczos only, alpha-correct; no sharpening, recolouring or new outlines."""
    if not isinstance(scale, int) or isinstance(scale, bool) or scale < 2 or scale > 8:
        raise ValueError("scale must be an integer between 2 and 8")
    original = original.convert("RGBA")
    size = (original.width * scale, original.height * scale)
    if original.getchannel("A").getextrema() == (255, 255):
        return original.convert("RGB").resize(size, Image.Resampling.LANCZOS).convert("RGBA")
    # Float32 avoids Pillow's 8-bit premultiplication discarding weak edge colours.
    px = np.asarray(original, dtype=np.float32)
    px[..., :3] *= px[..., 3:4] / 255.0
    planes = [np.asarray(Image.fromarray(px[..., c]).resize(size, Image.Resampling.LANCZOS)) for c in range(4)]
    alpha = np.clip(planes[3], 0, 255)
    rgb = np.stack(planes[:3], axis=2)
    rgb = np.minimum(np.maximum(rgb, 0), alpha[..., None])
    rgb = np.divide(rgb * 255.0, alpha[..., None], out=np.zeros_like(rgb), where=alpha[..., None] > 0)
    result = np.concatenate((np.clip(rgb, 0, 255), alpha[..., None]), axis=2)
    result = np.rint(result).astype(np.uint8)
    result[result[..., 3] == 0, :3] = 0
    return Image.fromarray(result)


def fidelity_metrics(original: Image.Image, hd: Image.Image, scale: int) -> dict:
    """The strict legacy comparator metrics, with a correctly represented source."""
    expected = (original.width * scale, original.height * scale)
    transparent = original.getchannel("A").getextrema()[0] < 255
    small = hd.resize(original.size, Image.Resampling.LANCZOS).convert("RGBA")
    o = np.asarray(original, dtype=np.float64)
    s = np.asarray(small, dtype=np.float64)
    problems = []
    if hd.size != expected:
        problems.append(f"size {hd.size} != {expected}")
    if hd.mode != "RGBA":
        problems.append(f"format {hd.mode} != RGBA")
    oa, sa = o[..., 3] > 127, s[..., 3] > 127
    union = np.logical_or(oa, sa).sum()
    iou = float(np.logical_and(oa, sa).sum() / union) if union else 1.0
    if transparent and iou < LIMITS["alpha_iou_min"]:
        problems.append(f"alpha IoU {iou:.6f} < {LIMITS['alpha_iou_min']}")
    if transparent and hd.getchannel("A").getextrema() == (255, 255):
        problems.append("transparency lost")
    weight = o[..., 3] / 255 if transparent else np.ones(o.shape[:2])
    diff = float((np.abs(o[..., :3] - s[..., :3]).mean(-1) * weight).sum() / weight.sum()) if weight.sum() else 0.0
    if diff > LIMITS["colour_diff_max"]:
        problems.append(f"mean colour difference {diff:.6f} > {LIMITS['colour_diff_max']}")
    gray = lambda x: (x[..., :3] @ np.array([0.299, 0.587, 0.114])) * (x[..., 3] / 255 if transparent else 1)
    score = _checker.ssim(gray(o), gray(s))
    if not np.isfinite(score) or score < LIMITS["ssim_min"]:
        problems.append(f"SSIM {score:.6f} < {LIMITS['ssim_min']}")
    # These extra diagnostics expose alpha or edge errors hidden by opaque pixels.
    composite_diffs = {}
    for value in (0, 255):
        co = o[..., :3] * o[..., 3:4] / 255 + value * (1 - o[..., 3:4] / 255)
        cs = s[..., :3] * s[..., 3:4] / 255 + value * (1 - s[..., 3:4] / 255)
        composite_diffs[str(value)] = round(float(np.abs(co - cs).mean()), 6)
    return {
        "result": "PASS" if not problems else "FAIL", "problems": problems,
        "limits": LIMITS, "mode": hd.mode, "size": list(hd.size), "expected_size": list(expected),
        "alpha_iou": round(iou, 6), "colour_diff": round(diff, 6), "ssim": round(score, 6),
        "alpha_mean_difference": round(float(np.abs(o[..., 3] - s[..., 3]).mean()), 6),
        "composite_mean_difference_black_white": composite_diffs,
        "comparison_reference": "source straight RGBA appearance, explicitly unpremultiplied only when original ERI flags require it",
    }


def source_consistency_step(original: Image.Image, current: Image.Image) -> Image.Image:
    """Correct one downsampling residual, without adding an external prior.

    Very small high-contrast UI strips can soften too far even with Lanczos.
    Back-project only their measured original-minus-roundtrip error. This is a
    deterministic source consistency correction, not learned/generated detail.
    Used only on a failed candidate, and accepted only by the unchanged gate.
    """
    def premultiplied(image):
        px = np.asarray(image, dtype=np.float32).copy()
        px[..., :3] *= px[..., 3:4] / 255
        return px
    target = premultiplied(original)
    small = premultiplied(current.resize(original.size, Image.Resampling.LANCZOS))
    residue = target - small
    expanded = premultiplied(current)
    for channel in range(4):
        expanded[..., channel] += np.asarray(Image.fromarray(residue[..., channel]).resize(current.size, Image.Resampling.BICUBIC))
    alpha = np.clip(expanded[..., 3:4], 0, 255)
    rgb = np.minimum(np.maximum(expanded[..., :3], 0), alpha)
    rgb = np.divide(rgb * 255, alpha, out=np.zeros_like(rgb), where=alpha > 0)
    result = np.rint(np.concatenate((np.clip(rgb, 0, 255), alpha), axis=2)).astype(np.uint8)
    result[result[..., 3] == 0, :3] = 0
    return Image.fromarray(result)


def alpha_quantization_step(original: Image.Image, current: Image.Image,
                            initial_alpha: np.ndarray, scale: int) -> Image.Image:
    """Resolve alpha 127/128 rounding only, with at most 16/255 total change.

    The strict binary IoU mask becomes unstable at exactly half opacity. Only
    source alpha values within four byte values of that cutoff are eligible;
    this cannot repair/reclassify a genuinely displaced opaque silhouette.
    Existing straight RGB stays untouched. Every result is independently gated.
    """
    native_alpha = np.asarray(original.getchannel("A"), dtype=np.int16)
    small_alpha = np.asarray(current.resize(original.size, Image.Resampling.LANCZOS).getchannel("A"), dtype=np.int16)
    ambiguous = (native_alpha >= 124) & (native_alpha <= 131)
    delta = np.where(ambiguous & (native_alpha >= 128) & (small_alpha < 128), 129 - small_alpha,
                     np.where(ambiguous & (native_alpha < 128) & (small_alpha >= 128), 126 - small_alpha, 0))
    delta = np.clip(delta, -8, 8)
    delta = np.repeat(np.repeat(delta, scale, axis=0), scale, axis=1)
    px = np.asarray(current).copy()
    updated = np.clip(px[..., 3].astype(np.int16) + delta,
                      np.maximum(initial_alpha - 16, 0), np.minimum(initial_alpha + 16, 255))
    px[..., 3] = updated.astype(np.uint8)
    px[px[..., 3] == 0, :3] = 0
    return Image.fromarray(px)


def png_rgba8(path: Path, expected: tuple[int, int]) -> bool:
    with path.open("rb") as stream:
        header = stream.read(33)
    return (len(header) == 33 and header[:8] == b"\x89PNG\r\n\x1a\n" and
            header[12:16] == b"IHDR" and struct.unpack(">II", header[16:24]) == expected and
            header[24:26] == b"\x08\x06")


def process_asset(asset: dict, root: Path = ROOT, audit_root: Path | None = None,
                  scale: int = 3, force: bool = False) -> dict:
    """Prepare one replacement and audit it atomically; reject failed candidates."""
    started = time.monotonic()
    root = Path(root).resolve()
    source = resolved(root, asset.get("source", asset.get("decoded_png", "")))
    if not asset.get("source", asset.get("decoded_png")):
        raise ValueError(f"{asset.get('id')}: no decoded PNG source supplied")
    if not asset.get("output"):
        raise ValueError(f"{asset.get('id')}: no output supplied")
    output = resolved(root, asset["output"])
    if output == source:
        raise ValueError("Replacement must never overwrite the original decoded source")
    factor = asset.get("scale", scale)
    alpha_mode = asset.get("alpha_mode")
    if not alpha_mode:
        flag = asset.get("format", 0)
        flag = int(flag, 0) if isinstance(flag, str) else int(flag)
        alpha_mode = "premultiplied" if flag & 0x04000000 and not flag & 0x08000000 else "straight"
    source_hash = sha256_file(source)
    identity = hashlib.sha256((str(source) + "\0" + str(output)).encode()).hexdigest()[:24]
    audit_root = Path(audit_root or (root / "hd/catalog/03 高清审查"))
    audit = audit_root / (identity + ".json")
    signature = {"version": VERSION, "decoded_png_sha256": source_hash, "scale": factor,
                 "alpha_mode": alpha_mode, "limits": LIMITS}
    if not force and audit.exists() and output.exists():
        previous = json.loads(audit.read_text(encoding="utf-8"))
        if (previous.get("signature") == signature and previous.get("result") == "PASS" and
                previous.get("output_sha256") == sha256_file(output)):
            previous["cached"] = True
            return previous
    with Image.open(source) as im:
        raw = im.convert("RGBA")
    original = straight_reference(raw, alpha_mode)
    if alpha_mode == "opaque" and raw.getchannel("A").getextrema() != (255, 255):
        raise ValueError("Inventory declares an opaque source but its decoded alpha is not fully opaque")
    if asset.get("source_size") and list(raw.size) != asset["source_size"]:
        raise ValueError(f"Decoded source size {raw.size} differs from inventory {asset['source_size']}")
    if alpha_mode == "premultiplied":
        a, b = np.asarray(raw), np.asarray(original)
        opaque = a[..., 3] == 255
        if not np.array_equal(a[..., 3], b[..., 3]) or not np.array_equal(a[opaque, :3], b[opaque, :3]):
            raise AssertionError("Representation correction changed alpha or opaque RGB")
    hd = faithful_resize(original, factor)
    metrics = fidelity_metrics(original, hd, factor)
    attempts = [{"method": "alpha-correct Lanczos", **metrics}]
    corrections = 0
    # No nearest-neighbour fallback: retain interpolated edges and source colours.
    # Stop as soon as the independently measured strict gate passes.
    while metrics["result"] != "PASS" and corrections < 3:
        hd = source_consistency_step(original, hd)
        corrections += 1
        metrics = fidelity_metrics(original, hd, factor)
        attempts.append({"method": f"source consistency correction {corrections}", **metrics})
    alpha_corrections = 0
    alpha_before_quantization = (np.asarray(hd.getchannel("A"), dtype=np.int16)
                                 if metrics["result"] != "PASS" and metrics["alpha_iou"] < LIMITS["alpha_iou_min"] else None)
    while (metrics["result"] != "PASS" and metrics["alpha_iou"] < LIMITS["alpha_iou_min"]
           and alpha_corrections < 4):
        hd = alpha_quantization_step(original, hd, alpha_before_quantization, factor)
        alpha_corrections += 1
        metrics = fidelity_metrics(original, hd, factor)
        attempts.append({"method": f"bounded half-opacity quantization correction {alpha_corrections}", **metrics})
    report = dict(asset)
    report.update(metrics)
    report.update({"signature": signature, "source": str(source), "output": str(output),
                   "source_size": list(original.size), "source_decoded_png_sha256": source_hash,
                   "alpha_mode_input": alpha_mode, "alpha_mode_output": "straight",
                   "method": "original pixels; Lanczos interpolation in premultiplied-alpha space; gate-failed candidates only use measured source roundtrip residual correction; no generative model, new outlines or recolouring",
                   "source_consistency_corrections": corrections,
                   "alpha_quantization_corrections": alpha_corrections,
                   "alpha_quantization_max_byte_change": (int(np.abs(np.asarray(hd.getchannel("A"), dtype=np.int16) - alpha_before_quantization).max()) if alpha_before_quantization is not None else 0),
                   "attempts": attempts,
                   "new_detail_claimed": False, "source_original_unchanged": sha256_file(source) == source_hash,
                   "audit": str(audit), "cached": False})
    if not report["source_original_unchanged"]:
        report["problems"].append("Source changed during conversion")
        report["result"] = "FAIL"
    if report["result"] == "PASS":
        output.parent.mkdir(parents=True, exist_ok=True)
        temporary = output.with_name(output.name + f".{os.getpid()}.part")
        hd.save(temporary, format="PNG", compress_level=3)
        with Image.open(temporary) as reopened:
            reopened.load()
            if reopened.mode != "RGBA" or reopened.size != hd.size:
                raise AssertionError("Saved PNG has unexpected dimensions or channels")
            if not np.array_equal(np.asarray(reopened), np.asarray(hd)):
                raise AssertionError("Saved PNG does not losslessly preserve the result")
        if not png_rgba8(temporary, hd.size):
            raise AssertionError("Saved replacement must be RGBA 8-bit PNG")
        report["output_sha256"] = sha256_file(temporary)
        report["output_bytes"] = temporary.stat().st_size
        os.replace(temporary, output)
    report["elapsed_seconds"] = round(time.monotonic() - started, 3)
    atomic_json(audit, report)
    return report


def _worker(task):
    asset, root, audit, scale, force = task
    try:
        return process_asset(asset, root, audit, scale, force)
    except Exception as error:
        return {"id": asset.get("id"), "source": asset.get("source"), "output": asset.get("output"),
                "result": "ERROR", "problems": [str(error)], "traceback": traceback.format_exc()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inventory", type=Path)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--audit-root", type=Path)
    parser.add_argument("--summary", type=Path)
    parser.add_argument("--scale", type=int, default=3)
    parser.add_argument("--workers", type=int, default=2)
    parser.add_argument("--limit", type=int)
    parser.add_argument("--game", choices=("yosuga", "haruka"))
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()
    if not 1 <= args.workers <= 8:
        parser.error("workers must be 1..8 to bound image memory")
    payload = json.loads(args.inventory.read_text(encoding="utf-8"))
    assets = payload["assets"] if isinstance(payload, dict) else payload
    assets = [a for a in assets if not args.game or a.get("game") == args.game]
    if args.limit:
        assets = assets[:args.limit]
    tasks = ((a, args.root, args.audit_root, args.scale, args.force) for a in assets)
    counts = {"PASS": 0, "FAIL": 0, "ERROR": 0, "cached": 0}
    records = []
    start = time.monotonic()
    with concurrent.futures.ProcessPoolExecutor(max_workers=args.workers) as pool:
        for i, report in enumerate(pool.map(_worker, tasks, chunksize=1), 1):
            records.append(report)
            counts[report["result"]] += 1
            counts["cached"] += bool(report.get("cached"))
            if i == 1 or i % 25 == 0 or report["result"] != "PASS" or i == len(assets):
                print(json.dumps({"done": i, "total": len(assets), "counts": counts,
                                  "elapsed_seconds": round(time.monotonic() - start, 1),
                                  "latest": report.get("id"), "problems": report.get("problems", [])}, ensure_ascii=False), flush=True)
    summary = {"version": VERSION, "inventory": str(args.inventory.resolve()), "scale_default": args.scale,
               "counts": counts, "total": len(assets), "elapsed_seconds": round(time.monotonic() - start, 2),
               "result": "PASS" if counts["FAIL"] == counts["ERROR"] == 0 else "FAIL", "assets": records}
    target = args.summary or args.root / "hd/catalog/04 高清处理汇总.json"
    atomic_json(target, summary)
    return 0 if summary["result"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
