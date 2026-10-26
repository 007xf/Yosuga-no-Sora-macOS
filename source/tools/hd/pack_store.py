#!/usr/bin/env python3
"""The packed HD store: every reviewed 4x companion as lossless WebP, deduplicated.

    pack_store.py build                    encode what is new into hd/store (parallel, resumable)
    pack_store.py install <hd dir>         write <hd dir>/manifest.tsv and clone the files it uses
    pack_store.py verify <hd dir> [--decode]
    pack_store.py verify-mods <mods dir> <hd dir>

Rows come from two reviewed sources:
  * the accepted bulk build (hd/bulk, '04 高清处理汇总.json'): the original game
    textures, minus the retired 晴空 skin PNGs (supplemental catalog 03) that no
    archive references any more;
  * the 空澄 skin build (mods/build/work/<kind>/<game>/validation.json): each
    texture's own 4x master, which reduces exactly to the PNG in its archive.

Encoding is lossless (cwebp -lossless -exact) and checked: every stored file
decodes to exactly the RGBA pixels of the reviewed PNG it replaces. Identical
pictures are stored once. The engine (image_codec.cpp) decodes the WebP to the
same straight RGBA as the PNG path.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
STORE = ROOT / "hd/store"
SUMMARY = ROOT / "hd/bulk/04 高清处理汇总.json"
RETIRED = ROOT / "hd/catalog/03 当前覆盖PNG素材.json"
SKIN_WORK = ROOT / "mods/build/work"
DENSITY = 4
CWEBP = shutil.which("cwebp") or "/opt/homebrew/bin/cwebp"
sys.path.insert(0, str(ROOT / "mods/src"))


def digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as source:
        for block in iter(lambda: source.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def pixels(image):
    rgba = image.convert("RGBA")
    h = hashlib.sha256(f"{rgba.width}x{rgba.height}:".encode())
    h.update(rgba.tobytes())
    return h.hexdigest(), rgba


def write_json(path, value):
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def load_index():
    path = STORE / "index.json"
    if path.exists():
        return json.loads(path.read_text(encoding="utf-8"))
    return {"version": 1, "files": {}, "png": {}}


def retired_sources():
    """The 晴空 skin PNGs an earlier build reviewed; no archive uses them any more."""
    if not RETIRED.exists():
        return set()
    return {asset["source_sha256"] for asset in json.loads(RETIRED.read_text(encoding="utf-8"))["assets"]}


def planned_rows():
    """(key, frame, width, height, source PNG path, expected PNG sha256, origin) for every runtime texture."""
    summary = json.loads(SUMMARY.read_text(encoding="utf-8"))
    if summary.get("result") != "PASS" or not summary.get("complete") or summary.get("scale") != DENSITY:
        raise SystemExit("The accepted HD build has not completely passed")
    retired = retired_sources()
    rows = []
    for asset in summary["assets"]:
        if asset["result"] != "PASS" or not asset["source_original_unchanged"]:
            raise SystemExit(f"accepted asset {asset['id']} is not a strict PASS")
        if asset["source_sha256"] in retired:
            continue
        w, h = asset["source_size"]
        rows.append((asset["source_sha256"], int(asset["frame"]), w, h, Path(asset["output"]),
                     asset["output_sha256"], "accepted"))
    import sky_ui  # the reduction that produced each archive texture
    import io
    for report in sorted(SKIN_WORK.glob("*/*/validation.json")):
        validation = json.loads(report.read_text(encoding="utf-8"))
        for item in validation["textures"].values():
            master = report.parent / "masters" / (item["sha256"] + ".png")
            w, h = item["size"]
            rows.append((item["sha256"], 0, w, h, master, None, f"skin:{report.parent.parent.name}/{report.parent.name}"))
    return rows, summary


def check_skin_master(master, sha, size):
    """The master must be exactly 4x and reduce to the very PNG stored in the archive."""
    import io
    import sky_ui
    with Image.open(master) as image:
        image.load()
        if image.size != (size[0] * DENSITY, size[1] * DENSITY):
            raise ValueError(f"{master}: not an exact 4x master")
        raw = io.BytesIO()
        sky_ui.reduce(image.convert("RGBA"), tuple(size)).save(raw, format="PNG")
    if hashlib.sha256(raw.getvalue()).hexdigest() != sha:
        raise ValueError(f"{master}: does not reduce to its archive texture")


def encode(job):
    """Encode one picture losslessly and prove it: returns (pixel sha, file sha, bytes)."""
    source, pixel_sha, target = job
    with tempfile.TemporaryDirectory() as scratch:
        out = Path(scratch) / "out.webp"
        subprocess.run([CWEBP, "-quiet", "-lossless", "-exact", "-z", "6", "-mt", str(source), "-o", str(out)],
                       check=True)
        with Image.open(source) as original, Image.open(out) as decoded:
            if pixels(decoded)[0] != pixel_sha or pixels(original)[0] != pixel_sha:
                raise ValueError(f"{source}: lossless WebP round trip differs")
        shutil.move(str(out), target)
    return pixel_sha, digest(target), target.stat().st_size


def build(workers):
    STORE.mkdir(parents=True, exist_ok=True)
    index = load_index()
    rows, summary = planned_rows()
    jobs, plan, checked = {}, [], set()
    for key, frame, w, h, source, expected, origin in rows:
        if not source.is_file():
            raise SystemExit(f"missing reviewed PNG {source}")
        png_sha = digest(source)
        if expected and png_sha != expected:
            raise SystemExit(f"{source}: bytes differ from the accepted read-back hash")
        if origin.startswith("skin") and png_sha not in checked:
            check_skin_master(source, key, (w, h))
            checked.add(png_sha)
        pixel_sha = index["png"].get(png_sha)
        if not pixel_sha:
            with Image.open(source) as image:
                if image.size != (w * DENSITY, h * DENSITY):
                    raise SystemExit(f"{source}: not exactly 4x {w}x{h}")
                pixel_sha = pixels(image)[0]
            index["png"][png_sha] = pixel_sha
        plan.append((key, frame, w, h, pixel_sha, origin))
        if pixel_sha not in index["files"] and pixel_sha not in jobs:
            jobs[pixel_sha] = (str(source), pixel_sha, STORE / (pixel_sha + ".webp"))
    print(json.dumps({"rows": len(plan), "unique_pictures": len({p[4] for p in plan}), "to_encode": len(jobs)}), flush=True)
    done = 0
    with concurrent.futures.ProcessPoolExecutor(workers) as pool:
        for pixel_sha, file_sha, size in pool.map(encode, jobs.values()):
            index["files"][pixel_sha] = {"file": pixel_sha + ".webp", "sha256": file_sha, "bytes": size}
            done += 1
            if done % 100 == 0:
                write_json(STORE / "index.json", index)
                print(f"encoded {done}/{len(jobs)}", flush=True)
    write_json(STORE / "index.json", index)
    identities = {}
    for row in plan:
        # Both games share some identical skin textures: same key, same picture.
        if (row[0], row[1]) in identities and identities[(row[0], row[1])][:5] != row[:5]:
            raise SystemExit(f"conflicting rows for {row[0]}:{row[1]}")
        identities[(row[0], row[1])] = row
    lines = ["\t".join((k, str(f), str(w), str(h), p)) for k, f, w, h, p, _ in sorted(identities.values())]
    (STORE / "rows.tsv").write_text("\n".join(lines) + "\n", encoding="utf-8")
    used = {row[4] for row in identities.values()}
    report = {"accepted_summary_sha256": digest(SUMMARY), "accepted_manifest_sha256": summary["manifest_sha256"],
              "rows": len(identities), "skin_rows": sum(r[5].startswith("skin") for r in identities.values()),
              "retired_rows": sum(1 for a in summary["assets"] if a["source_sha256"] in retired_sources()),
              "files": len(used), "bytes": sum(index["files"][p]["bytes"] for p in used),
              "rows_sha256": digest(STORE / "rows.tsv")}
    write_json(STORE / "report.json", report)
    print(json.dumps(report), flush=True)


def install(target):
    target = Path(target)
    if target.exists() and any(target.iterdir()):
        raise SystemExit(f"{target} is not empty; refusing to overwrite")
    target.mkdir(parents=True, exist_ok=True)
    index = load_index()
    rows = [line.split("\t") for line in (STORE / "rows.tsv").read_text(encoding="utf-8").splitlines() if line]
    manifest, used = [], set()
    for key, frame, w, h, pixel_sha in rows:
        record = index["files"][pixel_sha]
        manifest.append("\t".join((key, frame, w, h, record["file"])))
        used.add(pixel_sha)
    for pixel_sha in sorted(used):
        record = index["files"][pixel_sha]
        source, destination = STORE / record["file"], target / record["file"]
        # A plain copy, not an APFS clone: cwebp's output files carry generous
        # preallocated extents, which a clone would share into the app.
        shutil.copyfile(source, destination)
        if digest(destination) != record["sha256"]:
            raise SystemExit(f"{destination}: copy differs from the store")
    (target / "manifest.tsv").write_text("\n".join(manifest) + "\n", encoding="utf-8")
    print(json.dumps({"installed": str(target), "rows": len(manifest), "files": len(used)}))


def verify(directory, decode=False):
    directory = Path(directory).resolve()
    index = load_index()
    expected = {}
    for line in (STORE / "rows.tsv").read_text(encoding="utf-8").splitlines():
        key, frame, w, h, pixel_sha = line.split("\t")
        expected[(key, int(frame))] = (int(w), int(h), pixel_sha)
    by_file = {record["file"]: (pixel_sha, record) for pixel_sha, record in index["files"].items()}
    errors, seen, files, total = [], set(), set(), 0
    manifest = directory / "manifest.tsv"
    for number, line in enumerate(manifest.read_text(encoding="utf-8").splitlines(), 1):
        try:
            key, frame, w, h, name = line.split("\t")
            identity = (key, int(frame))
            if identity in seen:
                raise ValueError("duplicate identity")
            seen.add(identity)
            relative = Path(name)
            path = (directory / relative).resolve()
            if relative.is_absolute() or ".." in relative.parts or not path.is_relative_to(directory):
                raise ValueError("path leaves the HD directory")
            if identity not in expected or expected[identity][:2] != (int(w), int(h)):
                raise ValueError("row is not part of the reviewed set")
            pixel_sha, record = by_file[name]
            if pixel_sha != expected[identity][2]:
                raise ValueError("row points at a different picture")
            files.add(name)
        except Exception as error:
            errors.append({"line": number, "error": str(error)})
    for name in sorted(files):
        pixel_sha, record = by_file[name]
        path = directory / name
        if digest(path) != record["sha256"]:
            errors.append({"file": name, "error": "file bytes differ from the store"})
            continue
        total += path.stat().st_size
        if decode:
            with Image.open(path) as image:
                if pixels(image)[0] != pixel_sha:
                    errors.append({"file": name, "error": "decoded pixels differ"})
    actual = {p.name for p in directory.iterdir() if p.is_file() and p.name != "manifest.tsv"}
    if actual != files:
        errors.append({"error": "file set differs from manifest", "unlisted": sorted(actual - files)[:20],
                       "missing": sorted(files - actual)[:20]})
    if set(expected) != seen:
        errors.append({"error": "coverage differs from the reviewed set", "missing": len(set(expected) - seen),
                       "unexpected": len(seen - set(expected))})
    return {"result": "PASS" if not errors else "FAIL", "rows": len(seen), "files": len(files), "bytes": total,
            "decoded": decode, "errors": errors[:50]}


def verify_mods(mods, hd_dir):
    """Every bundled skin archive is the current build, and every PNG in it has its 4x companion."""
    import noa
    keys = set()
    for line in (Path(hd_dir) / "manifest.tsv").read_text(encoding="utf-8").splitlines():
        keys.add(line.split("\t")[0])
    errors, archives, pngs = [], 0, 0
    for archive in sorted(Path(mods).glob("*/*.noa")):
        archives += 1
        built = ROOT / "mods/build" / archive.parent.name / archive.name
        if not built.exists() or digest(built) != digest(archive):
            errors.append({"archive": str(archive), "error": "differs from the current skin build"})
        for entry in noa.read(archive)[1]:
            if entry.name.lower().endswith(".png") and entry.encoding == noa.STORED:
                data = entry.payload()[:entry.size]
                if not entry.name.endswith("_mask.png"):  # hit masks are never drawn
                    pngs += 1
                    if hashlib.sha256(data).hexdigest() not in keys:
                        errors.append({"archive": archive.name, "entry": entry.name, "error": "no HD companion"})
    return {"result": "PASS" if not errors and archives else "FAIL", "archives": archives, "pngs": pngs,
            "errors": errors}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    b = sub.add_parser("build")
    b.add_argument("--workers", type=int, default=5)
    i = sub.add_parser("install")
    i.add_argument("target")
    v = sub.add_parser("verify")
    v.add_argument("directory")
    v.add_argument("--decode", action="store_true")
    m = sub.add_parser("verify-mods")
    m.add_argument("mods")
    m.add_argument("hd")
    args = parser.parse_args()
    if args.command == "build":
        build(args.workers)
    elif args.command == "install":
        install(args.target)
    elif args.command == "verify":
        result = verify(args.directory, args.decode)
        print(json.dumps(result, ensure_ascii=False))
        return result["result"] != "PASS"
    else:
        result = verify_mods(args.mods, args.hd)
        print(json.dumps(result, ensure_ascii=False))
        return result["result"] != "PASS"


if __name__ == "__main__":
    sys.exit(main())
