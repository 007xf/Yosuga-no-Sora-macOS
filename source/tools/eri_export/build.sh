#!/bin/sh
# Build eri_export against a snapshot of the EntisGLS4 SDK static libraries.
# The snapshot (build/sdk_snapshot) is copied once from the vendor build so a
# concurrent vendor rebuild cannot break this tool; refresh with --refresh.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
VENDOR=$(cd "$HERE/../.." && pwd)/vendor/EntisGLS-SDL3
VB=$VENDOR/build/macos-sdl3-arm64
SNAP=$HERE/build/sdk_snapshot
if [ "$1" = "--refresh" ] || [ ! -f "$SNAP/lib/libgls4_sdl.a" ]; then
  mkdir -p "$SNAP/lib" "$SNAP/overlay"
  cp "$VB/libgls4_sdl.a" "$VB/libloquaty.a" "$VB/sdl3/libSDL3.a" "$VB/native/io/libentis_game_files.a" "$SNAP/lib/"
  rm -rf "$SNAP/overlay/Include" "$SNAP/overlay/include-revision"
  cp -R "$VB/sdk-sdl/Include" "$SNAP/overlay/"
  cp -R "$VB/sdl3/include-revision" "$SNAP/overlay/"
fi
CO=$VENDOR/EntisGLS/Cotopha/Include
mkdir -p "$HERE/build"
clang++ -std=gnu++17 -O2 -DNDEBUG -arch arm64 -mmacosx-version-min=11.0 \
  -DANDROID_NDK_VER=27 -DPLATFORM_ANDROID=0 -DSTUDYSTEADY_PLATFORM_SDL3=1 -DEXPORT_WITH_SDL_DECODER=1 \
  -Wno-invalid-offsetof -Wno-deprecated-declarations -fno-delete-null-pointer-checks \
  -I"$SNAP/overlay/Include" -I"$SNAP/overlay/Include/common" -I"$SNAP/overlay/Include/unix" -I"$SNAP/overlay/Include/opengl" \
  -I"$VENDOR/native" -I"$CO/common" -I"$CO/unix" -I"$CO/opengl" \
  -I"$SNAP/overlay/include-revision" -I"$VENDOR/vendor/sdl3/include" -I"$VENDOR/vendor/official-loquaty/Loquaty/include" \
  "$HERE/src/eri_export.cpp" -o "$HERE/build/eri_export" \
  "$SNAP/lib/libgls4_sdl.a" "$SNAP/lib/libloquaty.a" "$SNAP/lib/libentis_game_files.a" "$SNAP/lib/libSDL3.a" \
  -lz -liconv -framework OpenGL -framework Cocoa -framework IOKit -framework CoreAudio -framework AudioToolbox \
  -framework CoreVideo -framework Carbon -framework ForceFeedback -framework GameController -framework CoreHaptics \
  -framework Metal -framework QuartzCore -framework AVFoundation -framework CoreMedia -framework UniformTypeIdentifiers \
  -framework CoreFoundation -framework Foundation -framework Security -Wl,-weak_framework,QuartzCore
echo "built $HERE/build/eri_export"
