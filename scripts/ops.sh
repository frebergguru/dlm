#!/usr/bin/env bash
# Non-interactive exercise of the daemon queue operations that were fixed:
# add -> ls -> pause -> resume -> rm, against an isolated throwaway daemon.
# 'rm' while the item is active hits the deferred-remove/thread-join path.
#
# Usage:  scripts/ops.sh [URL]
set -uo pipefail

URL="${1:-https://proof.ovh.net/files/100Mb.dat}"

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
DLM="$(dlm_bin)"
dlm_isolated_env
trap dlm_cleanup EXIT

run() { echo; echo "\$ dlm $*"; "$DLM" "$@"; }

run add "$URL" -d "$DLM_WORK" -c 8
sleep 1; run ls
run pause 1; sleep 1; run ls
run resume 1; sleep 1; run ls
run rm 1;    sleep 1; run ls

echo
echo "done — if 'rm' left an empty list and nothing crashed, the fixed"
echo "remove/pause/resume paths are working."
