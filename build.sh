#!/usr/bin/env bash
#
# build.sh — configure, build and test dlm.
#
# Usage:
#   ./build.sh [extra cmake args...]
#
# Environment overrides:
#   BUILD_DIR=build      build directory
#   BUILD_TYPE=Release   CMake build type (Debug/Release/RelWithDebInfo)
#   JOBS=<n>             parallel build jobs (default: all cores)
#   NO_GUI=1             skip the GTK4/libadwaita GUI
#   ASAN=1              build with AddressSanitizer
#   KEEP_BUILD=1         keep the existing build tree (incremental); default is
#                        a clean build that wipes BUILD_DIR first
#   PREFIX=/usr/local    install prefix to configure now (./install.sh installs
#                        there later — the prefix is baked in at configure time)
#   NO_SYSTEMD=1         do not configure the systemd user service
#
set -euo pipefail
cd "$(dirname "$0")"

BUILD_DIR="${BUILD_DIR:-build}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="${JOBS:-$( (command -v nproc >/dev/null && nproc) || echo 4)}"

cmake_args=(-DCMAKE_BUILD_TYPE="$BUILD_TYPE")
[ "${NO_GUI:-0}" = "1" ] && cmake_args+=(-DDLM_GUI=OFF)
[ "${ASAN:-0}" = "1" ]  && cmake_args+=(-DDLM_ASAN=ON)
[ -n "${PREFIX:-}" ] && cmake_args+=(-DCMAKE_INSTALL_PREFIX="$PREFIX")
# Configure the install-time artifacts now (their paths are baked in at
# configure time); ./install.sh just copies them. Default the systemd unit on.
if [ "${NO_SYSTEMD:-0}" = "1" ]; then
  cmake_args+=(-DDLM_INSTALL_SYSTEMD=OFF)
else
  cmake_args+=(-DDLM_INSTALL_SYSTEMD=ON)
fi
cmake_args+=("$@")

# ---- dependency check (informative, non-fatal) --------------------------
echo "==> Checking dependencies"
missing=0
need_tool() { command -v "$1" >/dev/null 2>&1 || { echo "   MISSING tool: $1"; missing=1; }; }
need_pc()   { pkg-config --exists "$1" 2>/dev/null || { echo "   MISSING lib:  $1"; missing=1; }; }
need_tool cmake; need_tool pkg-config; need_tool cc || need_tool gcc
for m in libcurl jansson sqlite3 libpcre2-8 libcrypto; do need_pc "$m"; done
if [ "${NO_GUI:-0}" != "1" ]; then
  pkg-config --exists gtk4 libadwaita-1 2>/dev/null || \
    echo "   note: gtk4/libadwaita not found — GUI will be skipped"
fi
command -v yt-dlp >/dev/null 2>&1 || echo "   note: yt-dlp not found — needed at runtime for non-archive.org sites"
command -v ffmpeg >/dev/null 2>&1 || echo "   note: ffmpeg not found — needed by yt-dlp to merge streams"
if [ "$missing" = "1" ]; then
  echo
  echo "Install the missing build dependencies and re-run. On Arch/Manjaro:"
  echo "  sudo pacman -S --needed cmake pkgconf gcc curl jansson sqlite pcre2 openssl gtk4 libadwaita yt-dlp ffmpeg"
  exit 1
fi

# ---- clean build tree ---------------------------------------------------
# A clean build by default avoids stale CMake cache (e.g. a sticky option from a
# previous run) and root-owned artifacts left by a prior 'sudo ./install.sh'.
if [ "${KEEP_BUILD:-0}" != "1" ] && [ -e "$BUILD_DIR" ]; then
  echo "==> Cleaning $BUILD_DIR/"
  rm -rf "$BUILD_DIR" 2>/dev/null || true
  if [ -e "$BUILD_DIR" ]; then
    echo "   ERROR: could not remove $BUILD_DIR/ — likely root-owned files from a"
    echo "   prior 'sudo ./install.sh'. Remove it manually, then re-run:"
    echo "       sudo rm -rf $BUILD_DIR"
    exit 1
  fi
fi

# ---- configure + build + test ------------------------------------------
echo "==> Configuring ($BUILD_TYPE) in $BUILD_DIR/"
cmake -B "$BUILD_DIR" "${cmake_args[@]}"

echo "==> Building with $JOBS job(s)"
cmake --build "$BUILD_DIR" -j "$JOBS"

echo "==> Running tests"
ctest --test-dir "$BUILD_DIR" --output-on-failure

echo
echo "==> Done. Binaries in $BUILD_DIR/:"
for b in dlm dlmd dlm-gui; do
  [ -x "$BUILD_DIR/$b" ] && echo "     $BUILD_DIR/$b"
done
echo "    Install system-wide with: ./install.sh"
