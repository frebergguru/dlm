#!/usr/bin/env bash
# Build a self-contained Linux AppImage of the dlm GUI (bundling the GTK4 /
# libadwaita runtime). Expects the build dependencies to be installed already
# (see the package list in .github/workflows/release.yml or the README).
#
#   VERSION=1.2.3 ./installer/appimage/build-appimage.sh
#
# Output: dist/dlm-<version>-x86_64.AppImage
set -euo pipefail

cd "$(dirname "$0")/../.."          # repo root
VERSION=${VERSION:-$(git describe --tags --always 2>/dev/null || echo 0.1.0)}
VERSION=${VERSION#v}                # tags look like v1.2.3
export VERSION                      # linuxdeploy embeds this in the file name
export APPIMAGE_EXTRACT_AND_RUN=1   # run the tool AppImages without FUSE (CI/containers)
ARCH=x86_64
export ARCH

WORK=$PWD/build-appimage          # gitignored (build-*/)
echo ">>> building dlm (Release, GUI on)"
cmake -S . -B "$WORK" -DCMAKE_BUILD_TYPE=Release -DDLM_GUI=ON >/dev/null
cmake --build "$WORK" --target dlm-cli dlmd dlm-gui -j"$(nproc)"

echo ">>> assembling AppDir"
APPDIR=$WORK/AppDir
rm -rf "$APPDIR"
# dlm-gui spawns dlmd from its own directory, so both must sit in usr/bin.
install -Dm755 "$WORK/dlm-gui" "$APPDIR/usr/bin/dlm-gui"
install -Dm755 "$WORK/dlmd"    "$APPDIR/usr/bin/dlmd"
install -Dm755 "$WORK/dlm"     "$APPDIR/usr/bin/dlm"
# Full hicolor icon set (matches the desktop's Icon=guru.freberg.dlm); the
# desktop file itself is placed by linuxdeploy via --desktop-file below.
mkdir -p "$APPDIR/usr/share/icons"
cp -r assets/linux/hicolor "$APPDIR/usr/share/icons/"
ICON="$PWD/assets/linux/hicolor/256x256/apps/guru.freberg.dlm.png"

echo ">>> fetching linuxdeploy + gtk plugin + appimagetool"
wget -qO "$WORK/linuxdeploy.AppImage" \
    "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-$ARCH.AppImage"
wget -qO "$WORK/linuxdeploy-plugin-gtk.sh" \
    "https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gtk/master/linuxdeploy-plugin-gtk.sh"
wget -qO "$WORK/appimagetool.AppImage" \
    "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-$ARCH.AppImage"
chmod +x "$WORK/linuxdeploy.AppImage" "$WORK/linuxdeploy-plugin-gtk.sh" "$WORK/appimagetool.AppImage"

echo ">>> deploying GTK runtime into the AppDir"
# linuxdeploy finds the gtk plugin by name on PATH.
PATH="$WORK:$PATH" "$WORK/linuxdeploy.AppImage" \
    --appdir "$APPDIR" \
    --executable "$WORK/dlm-gui" \
    --executable "$WORK/dlmd" \
    --executable "$WORK/dlm" \
    --desktop-file "$PWD/packaging/dlm.desktop" \
    --icon-file "$ICON" \
    --plugin gtk

# linuxdeploy's excludelist drops libharfbuzz/libfribidi (assuming the host has
# them), which breaks on minimal systems. They're pure text-shaping libs with no
# host coupling, so bundle them explicitly. (X11/xcb/wayland/fontconfig/freetype
# stay on the host on purpose — bundling those causes display/font breakage.)
for lib in libharfbuzz.so.0 libfribidi.so.0; do
    path=$(ldconfig -p | awk -v l="$lib" '$1==l && /x86-64/ {print $NF; exit}')
    [ -n "$path" ] && cp -Lv "$path" "$APPDIR/usr/lib/"
done

echo ">>> packaging AppImage"
mkdir -p dist
APPIMAGETOOL_APP_NAME=dlm "$WORK/appimagetool.AppImage" \
    "$APPDIR" "dist/dlm-$VERSION-$ARCH.AppImage"
echo ">>> produced dist/dlm-$VERSION-$ARCH.AppImage"
