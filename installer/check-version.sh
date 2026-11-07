#!/usr/bin/env bash
# check-version.sh — fail unless the release version matches CMakeLists.txt.
#
# The single source of truth is project(dlm VERSION x.y.z) in CMakeLists.txt
# (compiled into the binaries as DLM_VERSION_STR). The version to check is, in
# order: the first argument, $VERSION, or the git tag in CI
# ($GITHUB_REF_NAME when $GITHUB_REF_TYPE is "tag"). A leading "v" is ignored.
# With none of those (e.g. a manual workflow run on a branch) it only prints
# the CMake version.
#
#   ./installer/check-version.sh            # CI: compare the pushed tag
#   ./installer/check-version.sh v0.6.0     # compare an explicit version
set -euo pipefail
cd "$(dirname "$0")/.."

cmake_version=$(sed -n 's/^project(dlm VERSION \([0-9][0-9.]*\).*/\1/p' CMakeLists.txt)
if [ -z "$cmake_version" ]; then
    echo "error: no project(dlm VERSION ...) in CMakeLists.txt" >&2
    exit 1
fi

want=${1:-${VERSION:-}}
if [ -z "$want" ] && [ "${GITHUB_REF_TYPE:-}" = tag ]; then
    want=${GITHUB_REF_NAME:-}
fi
if [ -z "$want" ]; then
    echo "CMake project version: $cmake_version (no tag/VERSION to compare)"
    exit 0
fi
want=${want#v}
if [ "$want" != "$cmake_version" ]; then
    echo "error: release version '$want' != CMake project version '$cmake_version'." >&2
    echo "       Bump project(dlm VERSION ...) in CMakeLists.txt or re-tag." >&2
    exit 1
fi
echo "version OK: $want"
