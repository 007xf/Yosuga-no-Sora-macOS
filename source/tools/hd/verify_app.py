#!/usr/bin/env python3
"""Read-only final app verification: the packed HD store, the skin archives, the
original runtime files, personal-save exclusion, version and signature."""
from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path
import plistlib
import re
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location("verify_app_store", Path(__file__).with_name("pack_store.py"))
STORE = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(STORE)


def verify_originals(games, catalog):
    records, errors = [], []
    archive_count, loose_count = 0, 0
    for source in catalog["archives"] + catalog["loose_files"]:
        relative = Path(source["relative"])
        path = games / source["game"] / relative
        # Only runtime assets are mandatory. Old executables, manuals, patch
        # archives and backup files in the read-only originals are not bundled.
        required = (source in catalog["archives"] or
                    source.get("role") == "live" and (relative.suffix.lower() in (".mei", ".csx") or relative.as_posix() == "system/input.xml"))
        if not path.is_file():
            if required:
                errors.append({"file": str(path), "error": "required original runtime file is missing"})
            continue
        if not path.resolve().is_relative_to(games.resolve()):
            errors.append({"file": str(path), "error": "runtime source resolves outside the bundled game directory"})
            continue
        actual = STORE.digest(path)
        ok = actual == source["sha256"] and path.stat().st_size == source["bytes"]
        records.append({"file": path.relative_to(games).as_posix(), "bytes": path.stat().st_size,
                        "sha256": actual, "result": "PASS" if ok else "FAIL"})
        if not ok:
            errors.append({"file": str(path), "error": "bundled source bytes differ from original census"})
        if source in catalog["archives"]:
            archive_count += 1
        else:
            loose_count += 1
    return {"result": "PASS" if not errors else "FAIL", "verified_noa_archives": archive_count,
            "verified_loose_runtime_files": loose_count, "files": records, "errors": errors}


def personal_files(app):
    violations = []
    names = {"system.dat", "config.dat", "progress.json"}
    saves = re.compile(r"(?i)^(?:(?:quick|auto|q)?save\d*|save\d{3})\.dat$")
    for path in app.rglob("*"):
        if not path.is_file():
            continue
        name = path.name.lower()
        if (path.suffix.lower() == ".sav" or name in names or saves.fullmatch(name)
                or any(part.lower() == "savedata" for part in path.relative_to(app).parts[:-1])):
            violations.append(path.relative_to(app).as_posix())
    return {"result": "FAIL" if violations else "PASS", "personal_save_files": violations}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("app", type=Path)
    parser.add_argument("--summary", type=Path, default=ROOT / "hd/bulk/04 高清处理汇总.json")
    parser.add_argument("--catalog", type=Path, default=ROOT / "hd/catalog/01 原始素材清单.json")
    parser.add_argument("--no-decode", action="store_true", help="skip decoding every HD file (hashes only)")
    parser.add_argument("--report", type=Path, help="optionally write this one JSON report; the app remains read-only")
    parser.add_argument("--expected-version", default="1.4.0", help="expected application release version")
    parser.add_argument("--expected-build", default="20261026.1", help="expected application build identifier")
    args = parser.parse_args()
    start = time.monotonic()
    app = args.app.resolve()
    report = {"app": str(app), "read_only": True, "checks": {}, "errors": []}
    try:
        if not app.is_dir():
            raise ValueError("App directory is missing")
        summary = json.loads(args.summary.read_text(encoding="utf-8"))
        catalog = json.loads(args.catalog.read_text(encoding="utf-8"))
        if STORE.digest(args.catalog) != summary["catalog_sha256"]:
            raise ValueError("Original source census differs from accepted HD build")
        report["accepted_summary"] = str(args.summary.resolve())
        resources = app / "Contents/Resources"
        for bundled_root in (resources, resources / "hd", resources / "games", resources / "games/mods"):
            if not bundled_root.resolve().is_relative_to(app):
                raise ValueError(f"Bundled resource root resolves outside the app: {bundled_root}")
        checks = report["checks"]
        for name, callback in (
            ("hd", lambda: STORE.verify(resources / "hd", decode=not args.no_decode)),
            ("active_mods", lambda: STORE.verify_mods(resources / "games/mods", resources / "hd")),
            ("original_runtime_files", lambda: verify_originals(resources / "games", catalog)),
            ("personal_saves", lambda: personal_files(app)),
        ):
            try:
                checks[name] = callback()
            except Exception as error:
                checks[name] = {"result": "FAIL", "error": str(error)}
        try:
            info = plistlib.loads((app / "Contents/Info.plist").read_bytes())
            checks["bundle_version"] = {"result": "PASS" if info.get("CFBundleShortVersionString") == args.expected_version and
                info.get("CFBundleVersion") == args.expected_build else "FAIL",
                "CFBundleShortVersionString": info.get("CFBundleShortVersionString"), "CFBundleVersion": info.get("CFBundleVersion")}
        except Exception as error:
            checks["bundle_version"] = {"result": "FAIL", "error": str(error)}
        try:
            signature = subprocess.run(["/usr/bin/codesign", "--verify", "--strict", str(app)], capture_output=True, text=True)
            checks["codesign"] = {"result": "PASS" if signature.returncode == 0 else "FAIL", "exit_code": signature.returncode,
                                  "stdout": signature.stdout.strip(), "stderr": signature.stderr.strip()}
        except Exception as error:
            checks["codesign"] = {"result": "FAIL", "error": str(error)}
    except Exception as error:
        report["errors"].append(str(error))
    report["result"] = "PASS" if not report["errors"] and report["checks"] and all(
        check["result"] == "PASS" for check in report["checks"].values()) else "FAIL"
    report["elapsed_seconds"] = round(time.monotonic()-start, 2)
    if args.report and args.report.resolve().is_relative_to(app):
        report["errors"].append("Report path must be outside the app so verification remains read-only")
        report["result"] = "FAIL"
    elif args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False))
    return report["result"] != "PASS"


if __name__ == "__main__":
    sys.exit(main())
