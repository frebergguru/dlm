#!/usr/bin/env bash
# Build the dlm Windows installers (both x86_64 and i686) inside the Fedora
# rawhide MinGW image defined by the sibling Dockerfile.
#
#   /src  : the dlm source tree (read-only mount)
#   /out  : where the finished installers are written
#
# For each architecture this: compiles libadwaita into the MinGW sysroot,
# cross-builds dlm.exe/dlmd.exe/dlm-gui.exe, gathers the full runtime DLL +
# GTK4 data closure, and runs makensis to produce a single setup .exe.
set -euo pipefail

SRC=${SRC:-/src}
OUT=${OUT:-/out}
VERSION=${VERSION:-$(git -C "$SRC" describe --tags --always 2>/dev/null || echo 0.1.0)}
LIBADWAITA_TAG=${LIBADWAITA_TAG:-1.7.7}
mkdir -p "$OUT"

# arch label -> mingw triplet / sysroot / NSIS define
if [ "${1:-}" = "--clean" ]; then
    echo "### cleaning intermediate build/stage dirs and installer outputs"
    rm -rf /work/build-* /work/stage-* /work/dlm.ico /work/i-*.png
    rm -f "$OUT"/*.exe
    echo "### clean"
    exit 0
fi

declare -A TRIPLET=([x64]=x86_64-w64-mingw32 [x86]=i686-w64-mingw32)
declare -A MESON_CPU=([x64]=x86_64 [x86]=x86)

makensis_bin=$(command -v makensis || true)
[ -n "$makensis_bin" ] || { echo "FATAL: makensis not found"; exit 1; }

# libadwaita is compiled into the sysroots at image-build time (prep-libadwaita.sh).
require_libadwaita() {
    local triplet=$1
    [ -f "/usr/$triplet/sys-root/mingw/lib/pkgconfig/libadwaita-1.pc" ] || {
        echo "FATAL: libadwaita missing for $triplet — rebuild the image"; exit 1; }
}

# Recursively copy the DLL closure of the given seed binaries from the sysroot
# bin into $STAGE/bin, skipping Windows system DLLs.
collect_dlls() {
    local triplet=$1 stage=$2; shift 2
    local sysbin=/usr/$triplet/sys-root/mingw/bin
    local objdump=${triplet}-objdump
    local -A seen=()
    local work=("$@")
    while [ ${#work[@]} -gt 0 ]; do
        local cur=${work[0]}; work=("${work[@]:1}")
        local imports
        imports=$("$objdump" -p "$cur" 2>/dev/null | awk '/DLL Name:/ {print $3}')
        local d
        for d in $imports; do
            local lc=${d,,}
            [ -n "${seen[$lc]:-}" ] && continue
            seen[$lc]=1
            if [ -f "$sysbin/$d" ]; then
                cp -n "$sysbin/$d" "$stage/bin/"
                work+=("$sysbin/$d")
            fi
        done
    done
}

build_arch() {
    local label=$1
    local triplet=${TRIPLET[$label]} cpu=${MESON_CPU[$label]}
    local sysroot=/usr/$triplet/sys-root/mingw
    local sysbin=$sysroot/bin
    local b=/work/build-$label stage=/work/stage-$label

    require_libadwaita "$triplet"

    echo "### cross-building dlm ($label)"
    rm -rf "$b" "$stage"
    local cmake_wrap=mingw64-cmake; [ "$label" = x86 ] && cmake_wrap=mingw32-cmake
    "$cmake_wrap" -S "$SRC" -B "$b" -DCMAKE_BUILD_TYPE=Release -DDLM_GUI=ON >/dev/null
    # app targets only (the POSIX-only test suite doesn't build on Windows)
    make -C "$b" dlm-cli dlmd dlm-gui -j"$(nproc)"

    echo "### staging runtime ($label)"
    mkdir -p "$stage/bin" "$stage/share" "$stage/lib"
    cp "$b"/dlm.exe "$b"/dlmd.exe "$b"/dlm-gui.exe "$stage/bin/"
    "${triplet}-strip" "$stage"/bin/*.exe

    # gdk-pixbuf loaders (incl. the SVG loader used for icons) + cache
    if [ -d "$sysroot/lib/gdk-pixbuf-2.0" ]; then
        cp -r "$sysroot/lib/gdk-pixbuf-2.0" "$stage/lib/"
    fi
    # GSettings schemas (GTK needs the compiled schema at runtime)
    mkdir -p "$stage/share/glib-2.0/schemas"
    cp -r "$sysroot"/share/glib-2.0/schemas/* "$stage/share/glib-2.0/schemas/" 2>/dev/null || true
    glib-compile-schemas "$stage/share/glib-2.0/schemas" || true
    # icon theme (the UI uses Adwaita symbolic icons) + hicolor index
    mkdir -p "$stage/share/icons"
    cp -r "$sysroot/share/icons/Adwaita" "$stage/share/icons/"
    [ -d "$sysroot/share/icons/hicolor" ] && cp -r "$sysroot/share/icons/hicolor" "$stage/share/icons/"
    # GTK looks up icons via the theme cache when present; (re)generate it
    command -v gtk4-update-icon-cache >/dev/null && \
        gtk4-update-icon-cache -q -t -f "$stage/share/icons/Adwaita" 2>/dev/null || true

    # DLL closure: seed with the exes AND the pixbuf loader plugins (loaded at
    # runtime, so their deps — e.g. librsvg — must be pulled in explicitly).
    local seeds=("$stage"/bin/*.exe)
    if [ -d "$stage/lib/gdk-pixbuf-2.0" ]; then
        while IFS= read -r -d '' f; do seeds+=("$f"); done \
            < <(find "$stage/lib/gdk-pixbuf-2.0" -name '*.dll' -print0)
    fi
    collect_dlls "$triplet" "$stage" "${seeds[@]}"
    echo "### staged $(find "$stage" -type f | wc -l) files, bin has $(ls "$stage"/bin/*.dll 2>/dev/null | wc -l) DLLs"

    echo "### makensis ($label)"
    local def64=0; [ "$label" = x64 ] && def64=1
    "$makensis_bin" -V2 \
        -DVERSION="$VERSION" -DARCH="$label" -DARCH64=$def64 \
        -DSTAGE="$stage" -DSRC="$SRC" -DICON=/work/dlm.ico \
        -DOUTFILE="$OUT/dlm-$VERSION-setup-$label.exe" \
        /work/dlm.nsi
    echo "### produced $OUT/dlm-$VERSION-setup-$label.exe"
}

make_icon() {
    [ -f /work/dlm.ico ] && return
    if [ -f "$SRC/assets/icon.ico" ]; then
        echo "### using assets/icon.ico"
        cp "$SRC/assets/icon.ico" /work/dlm.ico
        return
    fi
    echo "### generating dlm.ico from packaging/dlm.svg"
    local png=() s
    for s in 16 24 32 48 64 128 256; do
        rsvg-convert -w $s -h $s "$SRC/packaging/dlm.svg" -o /work/i-$s.png
        png+=(/work/i-$s.png)
    done
    icotool -c -o /work/dlm.ico "${png[@]}"
}

make_icon
for a in x64 x86; do build_arch "$a"; done
echo "### DONE"
ls -lh "$OUT"/*.exe
