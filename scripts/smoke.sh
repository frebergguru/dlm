#!/usr/bin/env bash
# End-to-end smoke test of the daemon path: builds, starts a throwaway daemon,
# queues a download, and opens the live watch view. Exercises the segmented
# engine plus the subscribe/progress (POLLOUT) path. Your real queue/socket are
# untouched, and everything is cleaned up on exit.
#
# Usage:  scripts/smoke.sh [URL]
#         (Ctrl-C to stop; the isolated daemon + temp dir are removed for you)
set -euo pipefail

URL="${1:-https://proof.ovh.net/files/100Mb.dat}"

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
DLM="$(dlm_bin)"
dlm_isolated_env
trap dlm_cleanup EXIT

echo "url:   $URL"
echo "work:  $DLM_WORK   (socket + db isolated here)"
echo

"$DLM" add "$URL" -d "$DLM_WORK" -c 8
echo
echo "watching — press Ctrl-C to stop and clean up"
"$DLM" watch
