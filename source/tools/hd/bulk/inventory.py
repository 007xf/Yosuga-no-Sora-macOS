#!/usr/bin/env python3
"""Inventory original Sphere assets without extracting or displaying artwork.

NOA directory/ERI headers are read directly. Only metadata and hashes are saved;
the original game roots are opened read-only. No image decode is necessary.
"""
from __future__ import annotations

import os
import argparse
from collections import Counter, defaultdict
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import struct
import sys

ROOT = Path(__file__).resolve().parents[3]
# The user's own installed games (read only). Default: folders beside the project.
GAME_ROOTS = {"yosuga": Path(os.environ.get("YOSUGA_DIR", ROOT.parent / "缘之空")),
              "haruka": Path(os.environ.get("HARUKA_DIR", ROOT.parent / "悠之空"))}
IMAGE_EXTS = {".eri", ".png", ".bmp", ".jpg", ".jpeg", ".tga", ".webp", ".gif", ".ico"}


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def file_hash(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(4 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def load_erisan():
    spec = importlib.util.spec_from_file_location("inventory_erisan", ROOT / "tools/noa/erisan.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.ERISANDecoder


def noa_directory(source, record_offset=0x40, data_base=0x40, prefix=""):
    """Offsets in a directory are relative to its container data, not 0x50."""
    source.seek(record_offset)
    tag, length = struct.unpack("<8sQ", source.read(16))
    if tag != b"DirEntry":
        raise ValueError(f"expected DirEntry at {record_offset:#x}, got {tag!r}")
    directory = source.read(length)
    count = struct.unpack_from("<I", directory)[0]
    pos = 4
    entries = []
    for _ in range(count):
        size, attr, encoding, offset = struct.unpack_from("<QIIQ", directory, pos)
        pos += 32
        extra_length, = struct.unpack_from("<I", directory, pos)
        pos += 4
        extra = directory[pos:pos + extra_length]
        pos += extra_length
        name_length, = struct.unpack_from("<I", directory, pos)
        pos += 4
        raw_name = directory[pos:pos + name_length].rstrip(b"\0")
        pos += name_length
        name = raw_name.decode("utf-8" if attr & 0x01000000 else "cp932")
        entries.append({"entry": prefix + name, "size": size, "attr": attr,
                        "encoding": encoding, "record_offset": data_base + offset,
                        "name_bytes_hex": raw_name.hex(), "extra_sha256": sha256(extra)})
    if pos != len(directory):
        raise ValueError(f"DirEntry trailing bytes: {len(directory) - pos}")
    result = []
    for entry in entries:
        if entry["attr"] & 0x10:
            result.extend(noa_directory(source, entry["record_offset"], entry["record_offset"] + 16,
                                        entry["entry"] + "/"))
        else:
            source.seek(entry["record_offset"])
            tag, length = struct.unpack("<8sQ", source.read(16))
            if tag != b"filedata":
                raise ValueError(f"expected filedata for {entry['entry']}, got {tag!r}")
            entry.update(payload_offset=entry["record_offset"] + 16, stored_bytes=length)
            result.append(entry)
    return result


def chunks(data, start=0x40, end=None):
    end = min(len(data), len(data) if end is None else end)
    while start + 16 <= end:
        tag, length = struct.unpack_from("<8sQ", data, start)
        if start + 16 + length > end:
            break
        yield tag, start + 16, length
        start += 16 + length


def media_metadata(data):
    if data[:6] != b"Entis\x1a":
        raise ValueError("missing Entis signature")
    result = {"container_identity": data[16:56].rstrip(b"\0").decode("ascii", "replace")}
    header = next(((pos, length) for tag, pos, length in chunks(data) if tag == b"Header  "), None)
    if not header:
        raise ValueError("missing ERI/MEI Header")
    start, length = header
    header_chunks = []
    for tag, pos, length in chunks(data, start, start + length):
        header_chunks.append(tag.decode("ascii", "replace").strip())
        payload = data[pos:pos + length]
        if tag == b"FileHdr " and length >= 20:
            ver, contained, keys, frames, duration = struct.unpack_from("<5I", payload)
            result.update(file_version=ver, contained_flags=contained, keyframes=keys,
                          frames=max(1, frames), duration_ms=duration)
        elif tag == b"ImageInf" and length >= 68:
            v = struct.unpack_from("<4I2i11I", payload)
            fmt = v[3]
            alpha = bool(fmt & 0x04000000)
            result.update(image_version=v[0], transformation=v[1], architecture=v[2], format=fmt,
                          width=abs(v[4]), height=abs(v[5]), signed_height=v[5],
                          source_size=[abs(v[4]), abs(v[5])], depth=v[6], clip_color=v[7],
                          sampling_flags=v[8], blocking_degree=v[13], lapped_block=v[14],
                          frame_transform=v[15], frame_degree=v[16], alpha=alpha,
                          alpha_mode=("straight" if fmt & 0x08000000 else "premultiplied") if alpha else "opaque",
                          palette=bool(fmt & 0x01000000), grayscale=(fmt & 0xFFFF) == 2)
        elif tag == b"descript":
            text = payload[2:].decode("utf-16-le", "replace") if payload[:2] == b"\xff\xfe" else payload.decode("utf-8", "replace")
            # Store only machine tags needed for placement, never human/story descriptions.
            tags = dict(re.findall(r"(?m)^#([^\r\n]+)\r?\n([^#]*?)(?=^#|\Z)", text, re.M | re.S))
            result["tags"] = {k: v.strip() for k, v in tags.items()
                              if k in {"hot-spot", "resolution", "reference-file", "rewind-point", "end-loop-point"}}
            hot = re.match(r"\s*(-?\d+)\s*,\s*(-?\d+)", result["tags"].get("hot-spot", ""))
            result["origin"] = [int(hot[1]), int(hot[2])] if hot else [0, 0]
            result["resolution_type"] = result["tags"].get("resolution")
        elif tag == b"Sequence":
            result["sequence_entries"] = length // 8
            result["sequence"] = [list(v) for v in struct.iter_unpack("<II", payload)]
        elif tag == b"SoundInf" and length >= 16:
            result["sound_header_hex"] = payload.hex()
    result["header_chunks"] = header_chunks
    result.setdefault("origin", [0, 0])
    result["stream_chunks"] = dict(Counter(tag.decode("ascii", "replace").strip()
        for outer, pos, length in chunks(data) if outer == b"Stream  "
        for tag, _, _ in chunks(data, pos, pos + length)))
    result["mip_levels"] = 1
    result["mip_note"] = "ERI image header has one stored size; size variants are separate archive entries."
    return result


def category(entry, archive):
    if entry.get("grayscale"):
        return "mask_or_grayscale"
    stem = Path(entry["entry"]).stem
    if archive.startswith("system/"):
        return "ui_" + Path(archive).stem
    if Path(archive).name == "data2.noa":
        return "sprite" if entry.get("alpha") else "effect_or_other_data2"
    if re.match(r"B\d", stem, re.I):
        return "background"
    if "wall" in stem.lower():
        return "wallpaper"
    return "cg_or_other_data1"


def summarize(assets):
    groups = defaultdict(list)
    for asset in assets:
        groups[asset["game"]].append(asset)
    result = {}
    for game, rows in groups.items():
        pixel_count = sum(a.get("width", 0) * a.get("height", 0) * a.get("frames", 1) for a in rows)
        hashes = {a["source_sha256"] for a in rows}
        result[game] = {"assets": len(rows), "unique_sources": len(hashes),
                        "frames": sum(a.get("frames", 1) for a in rows),
                        "animated_assets": sum(a.get("frames", 1) > 1 for a in rows),
                        "alpha_assets": sum(a.get("alpha", False) for a in rows),
                        "categories": dict(sorted(Counter(a["category"] for a in rows).items())),
                        "dimensions": dict(sorted(Counter(f"{a.get('width')}x{a.get('height')}" for a in rows).items())),
                        "source_pixels_all_frames": pixel_count,
                        "rgba_bytes_3x_uncompressed": pixel_count * 9 * 4,
                        "rgba_bytes_4x_uncompressed": pixel_count * 16 * 4}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=ROOT / "hd/catalog/01 原始素材清单.json")
    parser.add_argument("--verify-sources", action="store_true",
                        help="read an existing catalog and verify all original file hashes without writing")
    args = parser.parse_args()
    if args.verify_sources:
        catalog = json.loads(args.out.read_text(encoding="utf-8"))
        source_files = catalog["archives"] + catalog["loose_files"]
        mismatches = []
        for source in source_files:
            path = Path(source["path"])
            if not path.is_file() or path.stat().st_size != source["bytes"] or file_hash(path) != source["sha256"]:
                mismatches.append(str(path))
        print(json.dumps({"source_files": len(source_files), "verified": len(source_files) - len(mismatches),
                          "mismatches": mismatches}, ensure_ascii=False, indent=1))
        return bool(mismatches)
    decoder = load_erisan()
    catalog = {"schema_version": 1, "created_utc": datetime.now(timezone.utc).isoformat(),
               "method": "metadata-only NOA/ERI scan; no image rendering or generative processing",
               "original_roots_readonly": {k: str(v) for k, v in GAME_ROOTS.items()},
               "archives": [], "assets": [], "movies": [], "loose_files": [], "errors": []}
    for game, root in GAME_ROOTS.items():
        for path in sorted(root.rglob("*")):
            if not path.is_file():
                continue
            relative = path.relative_to(root).as_posix()
            print(f"scan {game}/{relative}", file=sys.stderr, flush=True)
            source_info = {"game": game, "path": str(path), "relative": relative,
                           "bytes": path.stat().st_size, "sha256": file_hash(path),
                           "role": "backup" if path.suffix.lower() == ".bak" else "live"}
            if path.suffix.lower() != ".noa":
                catalog["loose_files"].append(source_info)
                if path.suffix.lower() == ".mei":
                    try:
                        data = path.read_bytes()
                        catalog["movies"].append({**source_info, **media_metadata(data)})
                    except Exception as exc:
                        catalog["errors"].append({"file": str(path), "error": str(exc)})
                elif path.suffix.lower() in IMAGE_EXTS:
                    catalog["errors"].append({"file": str(path), "error": "loose graphic requires metadata support"})
                continue
            archive = {**source_info, "entries": []}
            try:
                with path.open("rb") as source:
                    entries = noa_directory(source)
                    for entry in entries:
                        source.seek(entry["payload_offset"])
                        encoded = source.read(entry["stored_bytes"])
                        if len(encoded) != entry["stored_bytes"]:
                            raise ValueError(f"truncated entry {entry['entry']}")
                        entry["encoded_sha256"] = sha256(encoded)
                        ext = Path(entry["entry"]).suffix.lower()
                        if ext in IMAGE_EXTS:
                            decoded = encoded if entry["encoding"] == 0 else decoder(encoded).decode(entry["size"])
                            if len(decoded) != entry["size"]:
                                raise ValueError(f"ERISAN length mismatch for {entry['entry']}")
                            meta = media_metadata(decoded)
                            asset = {"id": f"{game}/{relative}/{entry['entry']}", "game": game,
                                     "archive": str(path), "archive_relative": relative,
                                     "entry": entry["entry"], "source_sha256": sha256(decoded),
                                     "encoded_sha256": entry["encoded_sha256"],
                                     "source_bytes": entry["size"], "stored_bytes": entry["stored_bytes"],
                                     "encoding": entry["encoding"], "record_offset": entry["record_offset"],
                                     "payload_offset": entry["payload_offset"], **meta}
                            asset["category"] = category(asset, relative)
                            asset["metadata_sha256"] = sha256(json.dumps(asset, sort_keys=True, ensure_ascii=False).encode())
                            catalog["assets"].append(asset)
                            entry["asset_id"] = asset["id"]
                        entry["metadata_sha256"] = sha256(json.dumps(entry, sort_keys=True, ensure_ascii=False).encode())
                        archive["entries"].append(entry)
                archive["entry_count"] = len(archive["entries"])
                archive["extension_counts"] = dict(Counter(Path(e["entry"]).suffix.lower() for e in archive["entries"]))
            except Exception as exc:
                catalog["errors"].append({"file": str(path), "error": str(exc)})
            catalog["archives"].append(archive)
    catalog["summary"] = summarize(catalog["assets"])
    catalog["total_unique_image_sources"] = len({a["source_sha256"] for a in catalog["assets"]})
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(catalog, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
    print(json.dumps({"output": str(args.out), "summary": catalog["summary"],
                      "total_unique_image_sources": catalog["total_unique_image_sources"],
                      "movies": [{k: m.get(k) for k in ("game", "width", "height", "frames", "duration_ms")} for m in catalog["movies"]],
                      "errors": catalog["errors"]}, ensure_ascii=False, indent=1))
    return bool(catalog["errors"])


if __name__ == "__main__":
    sys.exit(main())
