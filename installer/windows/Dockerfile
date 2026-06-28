# Reproducible build environment for the dlm Windows installers.
#
# Fedora rawhide is the one distro that ships prebuilt MinGW-w64 GTK4 (4.22) and
# the full curl/jansson/openssl/pcre2/sqlite stack for BOTH 32-bit (mingw32) and
# 64-bit (mingw64) Windows, plus NSIS. The only GTK piece it lacks is the GNOME
# libadwaita library, which build.sh compiles from source into each sysroot.
#
#   docker build -t dlm-winbuild installer/windows
#   docker run --rm -v "$PWD":/src:ro -v "$PWD/dist":/out dlm-winbuild
#
# Output: dist/dlm-<version>-setup-x64.exe and ...-x86.exe
FROM fedora:rawhide

# Toolchains + prebuilt MinGW dependencies for both architectures, the native
# tools needed to compile libadwaita (meson/ninja/sassc/gettext) and to build
# the installer (nsis), and icon tooling (rsvg + icoutils for SVG->ICO).
RUN dnf -y --setopt=install_weak_deps=False install \
        mingw32-gcc mingw64-gcc \
        mingw32-gtk4 mingw64-gtk4 \
        mingw32-curl mingw64-curl \
        mingw32-jansson mingw64-jansson \
        mingw32-sqlite mingw64-sqlite \
        mingw32-pcre2 mingw64-pcre2 \
        mingw32-openssl mingw64-openssl \
        mingw32-appstream mingw64-appstream \
        mingw32-adwaita-icon-theme mingw64-adwaita-icon-theme \
        mingw32-nsis mingw64-nsis mingw-nsis-base \
        cmake ninja-build meson git sassc gettext glib2 glib2-devel \
        librsvg2-tools icoutils file findutils which \
    && dnf clean all

# Pin the libadwaita source revision compiled by build.sh (compatible with the
# GTK 4.22 in the image). Override with --build-arg if the GTK version moves.
ARG LIBADWAITA_TAG=1.7.7
ENV LIBADWAITA_TAG=${LIBADWAITA_TAG}

# Build the one missing GTK piece (libadwaita) into both sysroots, baked into
# the image so installer builds are fast and fully reproducible.
COPY prep-libadwaita.sh /work/
RUN bash /work/prep-libadwaita.sh

WORKDIR /work
COPY build.sh dlm.nsi /work/
RUN chmod +x /work/build.sh

ENTRYPOINT ["/work/build.sh"]
