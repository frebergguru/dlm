#!/usr/bin/env bash
# Diagnose a URL with the in-process engine (no daemon). Prints raw reachability
# (curl) and the real probe/engine result with debug logging, then downloads to
# a temp file. Use this when a daemon download goes straight to "error" — it
# tells you whether the cause is the network/URL or dlm itself.
#
# Usage:  scripts/diag.sh [URL]
set -uo pipefail

URL="${1:-https://proof.ovh.net/files/100Mb.dat}"

source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
DLM="$(dlm_bin)"

echo "== reachability (curl -I, following redirects) =="
if curl -sS -I -L "$URL" | grep -iE '^HTTP/|^location:|^content-length:|^accept-ranges:'; then
    :
else
    echo "  curl could not reach the host — likely DNS / network / host down."
fi
echo
echo "== in-process dlm get (DLM_LOG=debug) =="
tmp="$(mktemp -d)"
rc=0
DLM_LOG=debug "$DLM" get "$URL" -o "$tmp/dl.bin" -c 8 || rc=$?
echo
if [ "$rc" -eq 0 ]; then
    sz=$(stat -c %s "$tmp/dl.bin" 2>/dev/null || echo '?')
    echo "RESULT: OK — downloaded $sz bytes"
else
    echo "RESULT: FAILED (exit $rc) — see the 'probe failed' / error line above."
    echo "        If curl above also failed, it's the URL/network, not dlm."
fi
rm -rf "$tmp"
exit "$rc"
