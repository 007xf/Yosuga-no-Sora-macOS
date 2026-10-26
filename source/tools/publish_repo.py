#!/usr/bin/env python3
"""Assemble the source-only GitHub tree (fresh history, no game data).

    python3 -I tools/publish_repo.py <empty output dir> <screenshots dir> <app size> <hd size>

The tree mirrors this project under source/ so its scripts run unchanged:
source/vendor/EntisGLS-SDL3 (the committed engine snapshot), source/mods,
source/tools, source/design/icon. Only code and documentation from an
allowlist is copied; game archives, decoded or upscaled art, decompiled
scripts, dialogue, saves and the HD sets never leave this machine.
"""
from __future__ import annotations

import io
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[1]
ENGINE = ROOT / "vendor/EntisGLS-SDL3"
INCLUDE = [
    "mods/src/build_mods.py", "mods/src/make_frame_skin.py", "mods/src/make_logo_reference.py",
    "mods/src/noa.py", "mods/src/sky_ui.py",
    "tools/package_app.sh", "tools/fonts.swift", "tools/composite.py", "tools/ui_mockup.py", "tools/noa.py",
    "tools/noa/erisan.py", "tools/noa/noa_list.py", "tools/noa/cotomi_dump.py", "tools/noa/exe_cotomi.py",
    "tools/eri_export/build.sh", "tools/eri_export/noa_list.py", "tools/eri_export/src/eri_export.cpp",
    "tools/hd/README.md", "tools/hd/build_bulk.py", "tools/hd/bulk_upscale.py", "tools/hd/verify_bulk.py",
    "tools/hd/test_bulk_upscale.py", "tools/hd/check_hd.py", "tools/hd/pack_store.py", "tools/hd/verify_app.py",
    "tools/hd/dev_root.py",
    "tools/hd/bulk/inventory.py", "tools/hd/bulk/catalog_export.py", "tools/hd/bulk/test_runtime.py",
    "tools/progress/README.md", "tools/progress/build_manifest.py", "tools/progress/reference.py",
    "tools/progress/test_progress.py", "tools/progress/model_probe.cpp",
    "tools/ui_extract/contact_sheet.py", "tools/ui_extract/csx_funcs.py", "tools/ui_extract/pseudo.py",
    "tools/ui_extract/skin_inf.py",
    "design/icon/make_icon.py",
    "tools/build.sh", "tools/publish_repo.py", "tools/publish/README.md", "tools/publish/README.en.md",
    "HANDOFF.md",
]
SCREENSHOTS = ["banner", "01-title", "02-dialogue", "03-choice", "04-save", "05-config", "06-route-map",
               "07-route-overview", "08-gallery", "09-haruka", "10-history"]
# Text that would publish personal or game material if it slipped through.
FORBIDDEN = ["/Users/" + "sh/", "slack.com/" + "archives"]


def main():
    out, shots = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    app_size, hd_size = sys.argv[3], sys.argv[4]
    if out.exists() and any(out.iterdir()):
        raise SystemExit(f"{out} is not empty")
    source = out / "source"
    for relative in INCLUDE:
        src = ROOT / relative
        if not src.is_file():
            raise SystemExit(f"missing {relative}")
        tracked = subprocess.run(["git", "-C", str(ROOT), "ls-files", "--error-unmatch", relative],
                                 capture_output=True).returncode == 0
        if not tracked:
            raise SystemExit(f"{relative} is not committed")
        data = subprocess.run(["git", "-C", str(ROOT), "show", f"HEAD:{relative}"], capture_output=True,
                              check=True).stdout
        for word in FORBIDDEN:
            if word.encode() in data:
                raise SystemExit(f"{relative} contains {word}")
        target = source / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        if relative.endswith(".sh"):
            target.chmod(0o755)
    (source / "build.sh").write_bytes((source / "tools/build.sh").read_bytes())
    (source / "build.sh").chmod(0o755)
    (source / "tools/build.sh").unlink()
    for name in ("README.md", "README.en.md"):
        text = (source / "tools/publish" / name).read_text(encoding="utf-8")
        (out / name).write_text(text.replace("__APP_SIZE__", app_size).replace("__HD_SIZE__", hd_size), encoding="utf-8")
    shutil.rmtree(source / "tools/publish")
    images = out / "docs/images"
    images.mkdir(parents=True)
    for name in SCREENSHOTS:
        shutil.copy2(shots / f"{name}.jpg", images / f"{name}.jpg")
    archive = subprocess.run(["git", "-C", str(ENGINE), "archive", "--format=tar", "HEAD"], capture_output=True,
                             check=True).stdout
    with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
        tar.extractall(source / "vendor/EntisGLS-SDL3", filter="data")
    # Anchored: only the build outputs at the top of source/, never tools/hd etc.
    gitignore = ["/build/", "/work/", "/testgames/", "/logs/", "/shots/", "/hd/", "/mods/build/", "/design/assets/",
                 "/design/icon_src/", "/design/icon/AppIcon.*", "/analysis/", "/tools/eri_export/build/",
                 "/vendor/EntisGLS-SDL3/build/", "*.pyc", "__pycache__/", ".DS_Store"]
    (source / ".gitignore").write_text("\n".join(gitignore) + "\n", encoding="utf-8")
    print(out)


if __name__ == "__main__":
    main()
