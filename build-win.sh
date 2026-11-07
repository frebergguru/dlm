#!/usr/bin/env bash
# Build the Windows installers (64-bit and 32-bit) via Docker.
#
# Usage:
#   ./build-win.sh                 # build image if needed, then the installers
#   ./build-win.sh --rebuild       # force-rebuild the Docker image first
#   ./build-win.sh --clean         # remove the installers and the build image
#   VERSION=1.2.3 ./build-win.sh   # stamp a specific version
#
# Output: dist/dlm-<version>-setup-x64.exe and ...-x86.exe
set -euo pipefail

cd "$(dirname "$0")"
IMAGE=dlm-winbuild
CTX=installer/windows
BUILD_START=$(date +%s)             # every output must be newer than this

# Version = CMakeLists.txt project(dlm VERSION x.y.z). A VERSION from the
# environment (CI passes the tag) must match it; a leading "v" is ignored.
CMAKE_VERSION=$(sed -n 's/^project(dlm VERSION \([0-9][0-9.]*\).*/\1/p' CMakeLists.txt)
[ -n "$CMAKE_VERSION" ] || { echo "error: no project(dlm VERSION ...) in CMakeLists.txt" >&2; exit 1; }
VERSION=${VERSION:-$CMAKE_VERSION}
VERSION=${VERSION#v}
if [ "$VERSION" != "$CMAKE_VERSION" ]; then
    echo "error: VERSION '$VERSION' does not match the CMake project version '$CMAKE_VERSION'" >&2
    exit 1
fi
OUTPUTS=("dist/dlm-$VERSION-setup-x64.exe" "dist/dlm-$VERSION-setup-x86.exe")

command -v docker >/dev/null || { echo "error: docker not found" >&2; exit 1; }

rebuild=0
case "${1:-}" in
    "")        ;;
    --rebuild) rebuild=1 ;;
    --clean)
        echo ">>> removing installers and the build image"
        rm -f dist/*.exe
        rmdir dist 2>/dev/null || true
        if docker image rm -f "$IMAGE" >/dev/null 2>&1; then
            echo "    removed image $IMAGE"
        else
            echo "    image $IMAGE not present"
        fi
        echo ">>> clean done"
        exit 0 ;;
    *) echo "usage: $0 [--rebuild|--clean]" >&2; exit 2 ;;
esac

if [ "$rebuild" = 1 ] || ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo ">>> building image $IMAGE (first run compiles libadwaita; this is slow)"
    docker build -t "$IMAGE" "$CTX"
fi

mkdir -p dist
# Remove this version's old installers first so a failed build can never leave
# a stale one behind to be uploaded.
rm -f "${OUTPUTS[@]}"
if [ -e "${OUTPUTS[0]}" ] || [ -e "${OUTPUTS[1]}" ]; then
    echo "error: could not remove old installers in dist/ (root-owned?)" >&2; exit 1
fi

echo ">>> building installers ($VERSION)"
# Run build.sh / dlm.nsi from the source tree (not the copies baked into the
# image) so script fixes apply without an image rebuild.
docker run --rm \
    -v "$PWD":/src:ro -v "$PWD/dist":/out \
    -e VERSION="$VERSION" \
    --entrypoint /bin/bash \
    "$IMAGE" /src/installer/windows/build.sh

# Verify: both installers exist, were written by this run, and are stripped.
for f in "${OUTPUTS[@]}"; do
    [ -f "$f" ] || { echo "error: $f was not produced" >&2; exit 1; }
    if [ "$(stat -c %Y "$f")" -lt "$BUILD_START" ]; then
        echo "error: $f is older than this build — stale artifact" >&2; exit 1
    fi
done
bash ./check-stripped.sh "${OUTPUTS[@]}"

echo
echo ">>> done:"
ls -lh "${OUTPUTS[@]}"
