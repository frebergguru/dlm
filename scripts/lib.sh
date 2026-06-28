# Shared helpers for the dlm smoke scripts. Source this; don't run it directly.
#
#   dlm_repo            -> echoes the repo root (parent of scripts/)
#   dlm_bin             -> echoes the path to ./build/dlm, building if missing
#   dlm_isolated_env    -> exports XDG_RUNTIME_DIR + DLM_DB under a fresh tmpdir
#   dlm_cleanup         -> gracefully shuts the isolated daemon and removes it
#
# The point of the isolation: the daemon's socket lives at
# $XDG_RUNTIME_DIR/dlm.sock and its queue at $DLM_DB, so pointing both at a
# temp dir means these scripts never touch your real daemon, socket, or queue.

dlm_repo() { (cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd); }

dlm_bin() {
    local repo; repo="$(dlm_repo)"
    if [ ! -x "$repo/build/dlm" ]; then
        echo "building dlm..." >&2
        cmake --build "$repo/build" >&2
    fi
    echo "$repo/build/dlm"
}

dlm_isolated_env() {
    DLM_WORK="$(mktemp -d)"
    export DLM_WORK
    export XDG_RUNTIME_DIR="$DLM_WORK/run"
    export DLM_DB="$DLM_WORK/q.db"
    mkdir -p "$XDG_RUNTIME_DIR"
    chmod 700 "$XDG_RUNTIME_DIR"
}

dlm_cleanup() {
    set +e
    local sock="${XDG_RUNTIME_DIR:-}/dlm.sock"
    if [ -S "$sock" ]; then
        # graceful shutdown over the protocol (no 'dlm stop' subcommand exists)
        python3 - "$sock" 2>/dev/null <<'PY'
import socket, sys
try:
    s = socket.socket(socket.AF_UNIX); s.settimeout(2); s.connect(sys.argv[1])
    s.sendall(b'{"cmd":"shutdown"}\n'); s.recv(4096)
except Exception:
    pass
PY
        sleep 0.3
    fi
    # fallback: kill whatever still holds our isolated socket
    command -v fuser >/dev/null 2>&1 && fuser -k "$sock" 2>/dev/null
    [ -n "${DLM_WORK:-}" ] && rm -rf "$DLM_WORK"
}
