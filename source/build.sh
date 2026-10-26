#!/bin/zsh
# Builds 缘之空.app (缘之空 + 悠之空, native macOS) from your own installed copies
# of the two games (the Sphere 中文化委员会 Chinese releases). Nothing is
# downloaded and the game folders are only read.
#
#   ./build.sh "/path/to/缘之空" "/path/to/悠之空"
#
# Requirements: Apple silicon Mac, Xcode command line tools, Homebrew
#   brew install cmake bison webp
#   python3 -m pip install pillow numpy fonttools
# The faithful 4x HD set is made once from the games' own pixels and takes a
# few hours; later runs reuse it. The result is build/stage/缘之空.app.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")" && pwd)
[[ $# -eq 2 ]] || { print -u2 "usage: $0 <缘之空 folder> <悠之空 folder>"; exit 2; }
export YOSUGA_DIR=$(cd "$1" && pwd) HARUKA_DIR=$(cd "$2" && pwd)
for f in "$YOSUGA_DIR/system/frame.noa" "$HARUKA_DIR/system/frame.noa" "$YOSUGA_DIR/data1.noa" "$HARUKA_DIR/data1.noa"; do
    [[ -f "$f" ]] || { print -u2 "not an installed game folder: missing $f"; exit 1; }
done
BISON=$(brew --prefix bison 2>/dev/null)/bin/bison
for tool in cmake cwebp clang++ swift python3; do
    command -v $tool >/dev/null || { print -u2 "missing $tool (see the requirements above)"; exit 1; }
done
[[ -x "$BISON" ]] || { print -u2 "missing Homebrew bison (brew install bison)"; exit 1; }
python3 -c "import PIL, numpy, fontTools" 2>/dev/null || { print -u2 "python3 -m pip install pillow numpy fonttools"; exit 1; }

print "==> interface fonts (圆体-简, an Apple downloadable font)"
swift "$ROOT/tools/fonts.swift" >/dev/null

print "==> engine"
cd "$ROOT/vendor/EntisGLS-SDL3"
cmake -S . -B build/macos-sdl3-arm64 -DENTISGLS_LAUNCHER=ON -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 -DTJS_BISON="$BISON" >/dev/null
cmake --build build/macos-sdl3-arm64 --target studysteady_sdl --parallel
cd "$ROOT"

print "==> original interface art (decoded with the official SDK decoder)"
sh tools/eri_export/build.sh
for game in yosuga haruka; do
    dir=$([[ $game == yosuga ]] && print "$YOSUGA_DIR" || print "$HARUKA_DIR")
    for kind in frame title option; do
        [[ -f "design/assets/$game/$kind/manifest.json" ]] ||
            tools/eri_export/build/eri_export extract "$dir/system/$kind.noa" "design/assets/$game/$kind"
    done
done
[[ -f design/icon_src/CA01_01L.png ]] ||
    tools/eri_export/build/eri_export extract "$YOSUGA_DIR/data1.noa" design/icon_src --only CA01_01L.eri --no-frames

print "==> faithful 4x HD set (first run: a few hours)"
if [[ ! -f hd/bulk/manifest.tsv ]]; then
    python3 -I tools/hd/bulk/inventory.py
    python3 -I tools/hd/bulk/catalog_export.py --jobs 4
    python3 -I tools/hd/build_bulk.py --workers 4 --scale 4
    python3 -I tools/hd/verify_bulk.py hd/bulk --check-sources
fi

print "==> app icon"
[[ -f design/icon/AppIcon.icns ]] ||
    python3 design/icon/make_icon.py design/assets/yosuga/title/FRM_0611.png design/icon_src/CA01_01L.png

print "==> package"
tools/package_app.sh
