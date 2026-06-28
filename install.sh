#!/usr/bin/env bash
#
# install.sh — install the already-built dlm system-wide.
#
# This does NOT compile anything: run ./build.sh first, then this. It copies the
# built binaries (and the desktop/icon/systemd files) into the prefix the build
# was configured with, using sudo only when the prefix isn't writable.
#
# Usage:
#   ./build.sh                      # compile (prefix defaults to /usr/local)
#   ./install.sh                    # install there (prompts for sudo as needed)
#
#   PREFIX=~/.local ./build.sh      # compile for a user prefix...
#   ./install.sh                    # ...then install there (no sudo)
#
# Environment overrides:
#   BUILD_DIR=build      build directory to install from
#
set -euo pipefail
cd "$(dirname "$0")"

BUILD_DIR="${BUILD_DIR:-build}"

# Require a completed build — never compile here.
if [ ! -f "$BUILD_DIR/CMakeCache.txt" ] || [ ! -x "$BUILD_DIR/dlm" ]; then
  echo "error: no build found in '$BUILD_DIR/'. Compile first:"
  echo "    ./build.sh                    # default prefix /usr/local"
  echo "    PREFIX=~/.local ./build.sh    # or a custom prefix"
  exit 1
fi

# Install must use the prefix the build was configured with: the systemd unit's
# ExecStart path is baked in at configure time and can't be retargeted now.
prefix_from_cache() { sed -n 's/^CMAKE_INSTALL_PREFIX:[^=]*=//p' "$BUILD_DIR/CMakeCache.txt"; }
PREFIX_CACHED="$(prefix_from_cache)"; PREFIX_CACHED="${PREFIX_CACHED:-/usr/local}"
if [ -n "${PREFIX:-}" ] && [ "$PREFIX" != "$PREFIX_CACHED" ]; then
  echo "error: the build is configured for prefix '$PREFIX_CACHED', not '$PREFIX'."
  echo "       Re-compile with that prefix, then install:"
  echo "           PREFIX='$PREFIX' ./build.sh && ./install.sh"
  exit 1
fi
PREFIX="$PREFIX_CACHED"

# Use sudo only if we can't write to the prefix. Walk up to the nearest existing
# ancestor so a not-yet-created prefix is judged by its parent.
need_sudo() {
  local d="$1"
  while [ ! -e "$d" ]; do d="$(dirname "$d")"; done
  [ ! -w "$d" ]
}
SUDO=""
if need_sudo "$PREFIX"; then SUDO="sudo"; fi

echo "==> Installing to $PREFIX ${SUDO:+(with sudo)}"
$SUDO cmake --install "$BUILD_DIR"

# Refresh desktop/icon caches so the GUI shows up immediately (best effort).
$SUDO update-desktop-database "$PREFIX/share/applications" >/dev/null 2>&1 || true
$SUDO gtk-update-icon-cache -f "$PREFIX/share/icons/hicolor" >/dev/null 2>&1 || true

echo
echo "==> Installed. Binaries:"
for b in dlm dlmd dlm-gui; do
  [ -x "$PREFIX/bin/$b" ] && echo "     $PREFIX/bin/$b"
done
case ":$PATH:" in
  *":$PREFIX/bin:"*) ;;
  *) echo "    note: $PREFIX/bin is not on your PATH; add it to use 'dlm' directly." ;;
esac
echo
echo "Try:   dlm --help        |   launch 'dlm' from your application menu"
echo "Daemon: starts automatically on first use, or under systemd (if installed):"
echo "        systemctl --user enable --now dlm.service"
echo "Upgrade later: ./build.sh && ./install.sh, then 'dlm restart' to reload."
echo "Uninstall:     $SUDO cmake --build $BUILD_DIR --target uninstall"
