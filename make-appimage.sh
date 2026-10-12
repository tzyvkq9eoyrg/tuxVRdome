#!/usr/bin/env bash
# Builds tuxVRdome-<version>-x86_64.AppImage in this folder.
# Uses linuxdeploy (downloaded once into build-appimage/tools), which copies
# the libraries the player needs (libmpv, FFmpeg, OpenXR loader, …) into the
# image; graphics drivers, Vulkan and glibc come from the system as usual.
set -euo pipefail
cd "$(dirname "$0")"

VERSION=$(sed -n 's/^project(tuxvrdome VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
WORK=build-appimage
TOOL="$WORK/tools/linuxdeploy-x86_64.AppImage"

if [ ! -x "$TOOL" ]; then
  mkdir -p "$WORK/tools"
  curl -fL -o "$TOOL" \
    https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
  chmod +x "$TOOL"
fi

cmake -S . -B "$WORK/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build "$WORK/build" -j --target tuxvrdome
rm -rf "$WORK/AppDir"
DESTDIR="$PWD/$WORK/AppDir" cmake --install "$WORK/build"

# (Unpacks linuxdeploy instead of mounting it, so it runs without FUSE.)
echo "Sammle Bibliotheken ein, das dauert ein paar Minuten …"
APPIMAGE_EXTRACT_AND_RUN=1 LINUXDEPLOY_OUTPUT_VERSION="$VERSION" \
  "$TOOL" --appdir "$WORK/AppDir" --output appimage
echo "Fertig: $(ls tuxVRdome-"$VERSION"-*.AppImage)"
