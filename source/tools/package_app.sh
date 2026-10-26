#!/bin/zsh
# Builds 缘之空.app (Yosuga no Sora + Haruka na Sora) from the engine build,
# the mods and the user's game folders. Game data is cloned (APFS), never moved.
# usage: package_app.sh [<output dir>]   (default: build/stage)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
# The user's own installed games (read only). Default: folders beside the project.
YOSUGA=${YOSUGA_DIR:-"$ROOT/../缘之空"}
HARUKA=${HARUKA_DIR:-"$ROOT/../悠之空"}
ENGINE=$ROOT/vendor/EntisGLS-SDL3/build/macos-sdl3-arm64/EntisGLSLauncher.app
OUT=${1:-$ROOT/build/stage}
APP="$OUT/缘之空.app"

# This version is an HD-only package. Missing or incomplete HD artifacts must
# fail before rebuilding mods or replacing an existing staged application.
command -v cwebp >/dev/null || { print -u2 "cwebp is required (brew install webp)"; exit 1; }
if [[ ! -f "$ROOT/hd/bulk/manifest.tsv" || ! -f "$ROOT/hd/bulk/04 高清处理汇总.json" ]]; then
    print -u2 "HD packaging requires the complete accepted manifest and build summary."
    exit 1
fi

python3 -I $ROOT/mods/src/build_mods.py "$YOSUGA" "$HARUKA" $ROOT/mods/build >/dev/null

# The packed HD store: the accepted 4x set plus the skin's own 4x masters, as
# lossless WebP proven pixel-identical at encode time (incremental).
python3 -I "$ROOT/tools/hd/pack_store.py" build

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources/games/mods/yosuga" "$APP/Contents/Resources/games/mods/haruka"
cp "$ENGINE/Contents/MacOS/EntisGLSLauncher" "$APP/Contents/MacOS/Yosuga"
cp -R "$ENGINE/Contents/Resources/assets" "$APP/Contents/Resources/assets"
cp $ROOT/design/icon/AppIcon.icns "$APP/Contents/Resources/AppIcon.icns"

G="$APP/Contents/Resources/games"
clone_game() {  # <source dir> <name> <script>
    local src=$1 name=$2 csx=$3
    mkdir -p "$G/$name/system"
    for f in data1.noa data2.noa voice.noa sound.noa opening.mei; do cp -c "$src/$f" "$G/$name/$f"; done
    for f in $csx title.noa frame.noa option.noa input.xml; do cp -c "$src/system/$f" "$G/$name/system/$f"; done
}
clone_game "$YOSUGA" yosuga yosuga.csx
clone_game "$HARUKA" haruka Haruka.csx
for game in yosuga haruka; do
    for archive in title frame option; do cp "$ROOT/mods/build/$game/$archive.noa" "$G/mods/$game/$archive.noa"; done
done

python3 -I "$ROOT/tools/hd/pack_store.py" install "$APP/Contents/Resources/hd"

write_config() {  # <name> <script> <caption> [mod dir]
    local name=$1 csx=$2 caption=$3 mod=${4:-}
    {
        print '<?xml version="1.0" encoding="utf-8"?>'
        print "<script src=\"$csx\" id=\"$name\" profile=\"sphere-2008\" saves=\"appdata\" save_folder=\"$name\">"
        for f in data1 data2 voice sound; do print "\t<archive path=\"\$(CURRENT)\\\\$name\\\\$f.noa\"/>"; done
        [ -n "$mod" ] && print "\t<file path=\"\$(CURRENT)\\\\mods\\\\$name\"/>"
        print "\t<file path=\"\$(CURRENT)\\\\$name\\\\system\"/>"
        print "\t<file path=\"\$(CURRENT)\\\\$name\\\\\"/>"
        print "\t<display caption=\"$caption\" width=\"800\" height=\"600\" depth=\"32\" frequency=\"60\" CooperationLevel=\"window\"/>"
        print '</script>'
    } > "$G/$name.xml"
}
write_config yosuga yosuga.csx 缘之空 mod
write_config haruka Haruka.csx 悠之空 mod

cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleDevelopmentRegion</key><string>zh_CN</string>
	<key>CFBundleExecutable</key><string>Yosuga</string>
	<key>CFBundleIdentifier</key><string>local.sh.yosuganosora</string>
	<key>CFBundleName</key><string>缘之空</string>
	<key>CFBundleDisplayName</key><string>缘之空</string>
	<key>CFBundleIconFile</key><string>AppIcon</string>
	<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleShortVersionString</key><string>1.4.0</string>
	<key>CFBundleVersion</key><string>20261026.1</string>
	<key>LSMinimumSystemVersion</key><string>11.0</string>
	<key>LSApplicationCategoryType</key><string>public.app-category.adventure-games</string>
	<key>NSHighResolutionCapable</key><true/>
	<key>NSPrincipalClass</key><string>NSApplication</string>
	<key>NSHumanReadableCopyright</key><string>ヨスガノソラ・ハルカナソラ © Sphere. 简体中文化：Sphere 中文化委员会。EntisGLS © 1998-2025 理影, Entis soft.</string>
</dict>
</plist>
PLIST
print -n "APPL????" > "$APP/Contents/PkgInfo"

codesign --force --sign - "$APP" >/dev/null
# Read-only final verification includes strict signing, every packaged HD file
# (decoded), original runtime files, the copied skin archives and personal-save exclusion.
python3 -I "$ROOT/tools/hd/verify_app.py" "$APP"
du -sh "$APP" | cut -f1
print "$APP"
