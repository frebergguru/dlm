#!/usr/bin/env bash
# Compile libadwaita into both MinGW sysroots at image-build time. Fedora ships
# MinGW GTK4 but not the GNOME libadwaita library, so we build it once here from
# the tag pinned by the Dockerfile.
set -euo pipefail
TAG=${LIBADWAITA_TAG:-1.7.7}
src=/opt/libadwaita-src
git clone --depth 1 --branch "$TAG" https://gitlab.gnome.org/GNOME/libadwaita.git "$src"

build_one() { # $1=triplet $2=cpu_family
    local triplet=$1 cpu=$2 sysroot=/usr/$1/sys-root/mingw
    cat >/tmp/cross-$cpu.txt <<EOF
[binaries]
c = '${triplet}-gcc'
cpp = '${triplet}-g++'
ar = '${triplet}-ar'
strip = '${triplet}-strip'
pkg-config = '${triplet}-pkg-config'
windres = '${triplet}-windres'
[host_machine]
system = 'windows'
cpu_family = '${cpu}'
cpu = '${cpu}'
endian = 'little'
EOF
    meson setup "/tmp/_aw-$cpu" "$src" \
        --cross-file "/tmp/cross-$cpu.txt" \
        --prefix="$sysroot" --buildtype=release --wrap-mode=nodownload \
        -Dexamples=false -Dtests=false -Dvapi=false \
        -Dintrospection=disabled -Ddocumentation=false
    ninja -C "/tmp/_aw-$cpu" install
}

build_one x86_64-w64-mingw32 x86_64
build_one i686-w64-mingw32   x86
rm -rf /tmp/_aw-* /tmp/cross-*.txt "$src"
echo "libadwaita ${TAG} installed into both MinGW sysroots"
