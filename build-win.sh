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
echo ">>> building installers"
docker run --rm \
    -v "$PWD":/src:ro -v "$PWD/dist":/out \
    ${VERSION:+-e VERSION="$VERSION"} \
    "$IMAGE"

echo
echo ">>> done:"
ls -lh dist/*.exe
