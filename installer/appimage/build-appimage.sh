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
BUILD_START=$(date +%s)             # every output must be newer than this

# The version comes from CMakeLists.txt's project(dlm VERSION x.y.z), the same
# value compiled into the binaries (DLM_VERSION_STR). A VERSION from the
# environment (CI passes the tag) must match it exactly, or the release stops.
CMAKE_VERSION=$(sed -n 's/^project(dlm VERSION \([0-9][0-9.]*\).*/\1/p' CMakeLists.txt)
[ -n "$CMAKE_VERSION" ] || { echo "FATAL: no project(dlm VERSION ...) in CMakeLists.txt" >&2; exit 1; }
VERSION=${VERSION:-$CMAKE_VERSION}
VERSION=${VERSION#v}                # tags look like v1.2.3
if [ "$VERSION" != "$CMAKE_VERSION" ]; then
    echo "FATAL: VERSION '$VERSION' does not match the CMake project version '$CMAKE_VERSION'." >&2
    echo "       Bump project(dlm VERSION ...) in CMakeLists.txt or fix the tag." >&2
    exit 1
fi
export VERSION                      # linuxdeploy embeds this in the file name
export APPIMAGE_EXTRACT_AND_RUN=1   # run the tool AppImages without FUSE (CI/containers)
ARCH=x86_64
export ARCH

WORK="${WORK:-$PWD/build-appimage}"  # gitignored (build-*/); override to test
OUTPUT="dist/dlm-$VERSION-$ARCH.AppImage"

# Clean build tree: a stale CMakeCache.txt / old binaries / old AppDir must
# never end up in a release. Everything in $WORK is recreated below (the tool
# AppImages are re-downloaded each run). Refuse to wipe a directory that does
# not look like our build dir, in case WORK points somewhere unexpected.
if [ -e "$WORK" ]; then
    case "$(cd "$WORK" && pwd -P)" in
        /|"${HOME:-/}"|"$(pwd -P)") echo "FATAL: refusing to clean WORK=$WORK" >&2; exit 1 ;;
    esac
    if [ -n "$(ls -A "$WORK")" ] && [ ! -f "$WORK/CMakeCache.txt" ] && [ ! -d "$WORK/AppDir" ]; then
        echo "FATAL: WORK=$WORK is not empty and is not an AppImage build dir; refusing to clean it" >&2
        exit 1
    fi
    echo ">>> cleaning $WORK"
    rm -rf "$WORK"
fi
rm -f "$OUTPUT"                     # old artifact for this version

echo ">>> building dlm (Release, GUI on)"
# DLM_NATIVE stays OFF: the AppImage must run on any x86-64 CPU.
cmake -S . -B "$WORK" -DCMAKE_BUILD_TYPE=Release -DDLM_GUI=ON -DDLM_NATIVE=OFF >/dev/null
cmake --build "$WORK" --target dlm-cli dlmd dlm-gui -j"$(nproc)"

# The freshly built CLI must report the version we are stamping.
built_version=$("$WORK/dlm" version)
if [ "$built_version" != "dlm $VERSION" ]; then
    echo "FATAL: built binary reports '$built_version', expected 'dlm $VERSION'" >&2
    exit 1
fi

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

# The gtk plugin's copy_lib_tree assumes a /usr/lib/gtk-4.0 modules dir exists,
# but Arch/Manjaro (and other distros) don't ship one — it then aborts. Make the
# copy skip module dirs that aren't present. (No-op where the dir does exist.)
if ! grep -q 'e "\$elem" \]\ ||\ continue' "$WORK/linuxdeploy-plugin-gtk.sh"; then
    sed -i '/for elem in "\${src\[@\]}"; do/a\        [ -e "$elem" ] || continue   # skip absent module dirs (e.g. no /usr/lib/gtk-4.0)' \
        "$WORK/linuxdeploy-plugin-gtk.sh"
fi

# linuxdeploy bundles an old strip that can't parse the modern .relr.dyn ELF
# section emitted by current toolchains; let it skip stripping and we strip with
# the host's strip below instead.
export NO_STRIP=1

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
# Snapshot the linker cache once: piping `ldconfig -p` into an awk that exits on
# first match closes the pipe early, and SIGPIPE on ldconfig is fatal under
# `set -o pipefail`. Reading from a here-string avoids the live-pipe SIGPIPE.
ldcache=$(ldconfig -p)
for lib in libharfbuzz.so.0 libfribidi.so.0; do
    path=$(awk -v l="$lib" '$1==l && /x86-64/ {print $NF; exit}' <<<"$ldcache")
    [ -n "$path" ] && cp -Lv "$path" "$APPDIR/usr/lib/" || true
done

# Strip with the host toolchain (handles .relr.dyn, unlike linuxdeploy's bundled
# strip). Every ELF in the AppDir is stripped — our binaries and every bundled
# .so / loader module. A failed strip is reported (never silenced), and the
# check-stripped.sh run below stops the release if any file is left unstripped.
echo ">>> stripping bundled binaries"
while IFS= read -r -d '' f; do
    if [ "$(head -c4 "$f" | od -An -tx1 | tr -d ' \n')" = "7f454c46" ]; then
        chmod u+w "$f"
        case "$f" in
            *.so|*.so.*) strip --strip-unneeded "$f" || echo "   warning: strip failed: $f" >&2 ;;
            *)           strip --strip-all "$f"      || echo "   warning: strip failed: $f" >&2 ;;
        esac
    fi
done < <(find "$APPDIR" -type f -print0)

# Enforced: refuse to pack if anything is still unstripped.
echo ">>> checking that every bundled binary is stripped"
bash ./check-stripped.sh "$APPDIR" >"$WORK/check-stripped.log" || {
    grep -v '^  stripped ' "$WORK/check-stripped.log" >&2; exit 1; }
tail -n1 "$WORK/check-stripped.log"

echo ">>> packaging AppImage"
mkdir -p dist
APPIMAGETOOL_APP_NAME=dlm "$WORK/appimagetool.AppImage" \
    "$APPDIR" "$OUTPUT"

# Verify the artifact: present, freshly written by this run, and stripped
# (the AppImage runtime is an ELF too).
[ -f "$OUTPUT" ] || { echo "FATAL: $OUTPUT was not produced" >&2; exit 1; }
if [ "$(stat -c %Y "$OUTPUT")" -lt "$BUILD_START" ]; then
    echo "FATAL: $OUTPUT is older than this build — stale artifact" >&2; exit 1
fi
bash ./check-stripped.sh "$OUTPUT" >/dev/null || { bash ./check-stripped.sh "$OUTPUT" >&2; exit 1; }
echo ">>> produced $OUTPUT"
