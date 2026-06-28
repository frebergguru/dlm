# Architecture

## Components

| Component | Source | Role |
|-----------|--------|------|
| **libdlm** | `libdlm/` | Core library: download engine, queue store, extractors, checksums, auto-tools, compat layer. Linked statically by all binaries. |
| **dlmd** | `daemon/` | Long-lived daemon. Owns the queue/scheduler, serves the IPC socket, drives worker threads, and owns tool management. |
| **dlm** | `cli/` | CLI client. `get` runs a download in-process; other verbs talk to `dlmd`. |
| **dlm-gui** | `gui/` | GTK4 + libadwaita reactive view over `dlmd`. |

### libdlm internals

- `download.c` — segmented HTTP(S) engine on curl-multi: probes size/range
  support, plans N segments, writes each at its absolute offset
  (`dlm_pwrite`), journals per-segment progress to a `.dlmjson` sidecar for
  resume, then atomically renames the `.dlmpart` file into place.
- `store.c` — SQLite persistence of the queue/packages.
- `extract.c` + `extractors/` — URL → concrete tasks. `archiveorg.c` uses the
  Internet Archive APIs natively; `ytdlp.c` shells out to `yt-dlp -J` and turns
  the JSON into engine tasks (or marks them `delegate` for yt-dlp to download).
- `httpget.c` — small in-memory HTTP GET/POST helper (used by extractors, IA
  auth, and the tool version/checksum fetches).
- `verify.c` — MD5 / SHA-1 / SHA-256 via OpenSSL EVP.
- `iaauth.c` — Internet Archive credential storage + login.
- `tools.c` — auto-managed external tools (see [auto-tools.md](auto-tools.md)).
- `compat/` — platform-compatibility layer (see [compat.md](compat.md)).

## Process model & data flow

1. A client (`dlm` or `dlm-gui`) connects to the daemon's AF_UNIX socket,
   auto-spawning `dlmd` (detached) if it is not already running.
2. Requests/responses are newline-delimited JSON ([protocol.md](protocol.md)).
3. `dlmd` runs a single `poll()` loop (`dlm_poll`) multiplexing the listening
   socket and all clients; it ticks the scheduler every 200 ms and pushes a
   `progress` event to subscribed clients every 0.5 s.
4. The scheduler starts a worker thread per active download. A worker either
   drives the segmented engine directly, or — for `delegate` tasks — spawns
   `yt-dlp` and parses its progress lines.
5. `dlm get` bypasses the daemon and runs the engine in-process.

## On-disk locations

Paths come from the compat layer (`libdlm/compat/dlm_paths.c`) so the daemon and
clients always agree.

| Item | Linux | Windows |
|------|-------|---------|
| IPC socket | `$XDG_RUNTIME_DIR/dlm.sock` (else `/tmp/dlm-<uid>.sock`) | `%LOCALAPPDATA%\dlm\dlm.sock` |
| Queue DB | `$XDG_DATA_HOME/dlm/queue.db` (else `~/.local/share/dlm/queue.db`); `$DLM_DB` overrides | `%LOCALAPPDATA%\dlm\queue.db` |
| Managed tools | `$XDG_DATA_HOME/dlm/tools/` | `%LOCALAPPDATA%\dlm\tools\` |
| Tool state | `<data>/dlm/tools/state.json` | same |
| IA credentials | `$XDG_CONFIG_HOME/dlm/credentials` (else `~/.config/dlm/...`) | `%APPDATA%\dlm\credentials` |
| Favicon cache (GUI) | `$XDG_CACHE_HOME/dlm/favicons/` | `%LOCALAPPDATA%\dlm\cache\favicons\` |

## Lifecycle of a download

```
URL ──dlm_extract──> tasks ──┬─ progressive http  ──> engine (segmented, resume, verify) ──> file
                             └─ HLS/DASH/merge     ──> yt-dlp (delegate)               ──> file
```

The daemon persists queue state across restarts; an interrupted `active`
download is requeued and resumes from its journal.
