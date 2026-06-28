# Protocols

`dlm` uses two line/stream protocols:

1. The **daemon IPC protocol** — between the clients (`dlm`, `dlm-gui`) and
   `dlmd`.
2. The **yt-dlp progress protocol** — an internal line format the daemon asks
   `yt-dlp` to emit so it can render live progress for delegated downloads.

---

## 1. Daemon IPC protocol

### Transport

- **Socket:** `AF_UNIX` / `SOCK_STREAM` at the path in
  [architecture.md](architecture.md) (Winsock AF_UNIX on Windows).
- **Framing:** one JSON object per line, terminated by `\n`. A request line gets
  exactly one response line. Subscribed clients additionally receive
  asynchronous event lines.
- **Version:** `DLM_PROTO_VERSION = 3`. On connect, a client sends `ping`; if the
  daemon's `proto` differs, the client shuts the old daemon down and respawns a
  matching one.

### Request envelope

```json
{"cmd": "<name>", ...args}
```

### Response envelope

Success carries `"ok": true` (plus command-specific fields). Failure:

```json
{"ok": false, "error": "<message>"}
```

Unknown command → `{"ok":false,"error":"unknown cmd"}`; unparseable line →
`{"ok":false,"error":"invalid json"}`.

### Commands

| cmd | Request fields | Response | Notes |
|-----|----------------|----------|-------|
| `ping` | — | `{ok, pong:"dlmd", proto:int}` | version handshake |
| `add` | `url` (req), `out?`, `connections?`, `delegate?` | `{ok, id}` | enqueue a single URL |
| `grab` | `name?`, `folder?`, `links:[{url, out?, name?, size?, connections?, delegate?, availability?}]` | `{ok, package_id}` | stage a package in the linkgrabber |
| `confirm` | `id`, `package?`, `start?` (default true) | `{ok, moved:int}` | move linkgrabber item(s) to the download list |
| `lg_remove` | `id`, `package?` | `{ok, removed:int}` | remove a linkgrabber item |
| `lg_clear` | — | `{ok, removed:int}` | clear the whole linkgrabber |
| `list` | — | `{ok, max_active, max_speed, autostart, tools, packages[], downloads[]}` | full snapshot |
| `subscribe` | — | same payload as `list`, then a stream of `progress` events | long-lived connection |
| `pause` / `cancel` | `id` | `{ok}` | stop, keep resumable |
| `resume` | `id` | `{ok}` | resume a paused/errored item |
| `rm` | `id`, `package?` | `{ok}` | remove from the queue |
| `priority` | `id`, `package?`, `level` | `{ok}` | set priority level |
| `enable` / `disable` | `id`, `package?` | `{ok}` | toggle whether an item may start |
| `autostart` | `id`, `package?`, `on?` | `{ok}` | per-item autostart |
| `force` | `id`, `package?` | `{ok}` | start now, ignoring limits |
| `move` | `id`, `package?`, `dir`: `up`\|`down`\|`top`\|`bottom` | `{ok}` | reorder |
| `pkg` | `id`, `name?`, `folder?`, `comment?`, `priority?`, `collapsed?` | `{ok}` | edit a package |
| `clear_finished` | — | `{ok, removed:int}` | drop completed items |
| `set` | `max_active?`, `max_speed?` (bytes/s, 0 = unlimited), `autostart?`, `auto_tools?` | `{ok, max_active, max_speed, autostart, tools}` | update global settings |
| `tools_update` | — | `{ok}` | trigger a forced yt-dlp update check (runs off-thread) |
| `shutdown` | — | `{ok}` | stop the daemon |

`id` is the integer id returned by `add`/`grab` (or from a snapshot). `package?`
booleans indicate the id refers to a package rather than a single download.

### Download object (in `downloads[]`)

```json
{
  "id": 12, "url": "...", "out_path": "...", "name": "...",
  "connections": 4, "total": 12345678, "downloaded": 2345678,
  "speed": 1048576.0, "state": "active",
  "package_id": 3, "priority": 0,
  "enabled": true, "autostart": true, "force": false,
  "list": "download", "availability": "online",
  "delegate": false, "error": "..."
}
```

- `state`: `queued` | `active` | `paused` | `done` | `error`.
- `list`: `download` | `linkgrabber`.
- `total` is `-1` when unknown; `error` is present only on failures.

### Package object (in `packages[]`)

```json
{
  "id": 3, "name": "...", "folder": "...", "comment": "...",
  "list": "download", "priority": 0, "collapsed": false, "links": 5
}
```

### `tools` object (in `list` / `set` responses)

```json
{
  "yt-dlp":  {"managed": true,  "present": true, "version": "2026.06.09"},
  "ffmpeg":  {"managed": false, "present": true},
  "ffprobe": {"managed": false, "present": true},
  "auto": true
}
```

- `managed`: a copy lives in dlm's tools dir.
- `present`: usable (managed **or** on `PATH`).
- `auto`: auto-download/update enabled.

### `progress` event (pushed to subscribers ~every 0.5 s)

```json
{
  "event": "progress",
  "max_active": 3, "max_speed": 0, "autostart": true,
  "packages": [ ... ], "downloads": [ ... ]
}
```

Same `packages`/`downloads` shapes as `list`. Clients re-render from each event.

---

## 2. yt-dlp progress protocol (internal)

For a delegated download the daemon runs:

```
yt-dlp --no-warnings --no-playlist --newline --progress \
       --progress-template "<TEMPLATE>" \
       [--ffmpeg-location <dir>] [--limit-rate <bps>] \
       [--merge-output-format <ext>] -o <out_path> -- <url>
```

where the template (`DLM_YTDLP_TMPL` in `daemon/queue.c`) is:

```
download:dlmprog|%(progress.downloaded_bytes)s|%(progress.total_bytes)s|%(progress.total_bytes_estimate)s|%(progress.speed)s
```

yt-dlp then prints, per tick, lines of the form:

```
dlmprog|<downloaded>|<total>|<total_estimate>|<speed>
```

Each field is an integer/float, or the literal `NA` when unknown. The daemon
parses these (`parse_progress_line`) to update the item's `downloaded`, `total`
(falling back to the estimate), and `speed`, which then flow out via the
`progress` event above. Non-matching lines (yt-dlp's own logging) are ignored.

The extractor path uses a separate one-shot invocation, `yt-dlp -J ... <url>`,
whose JSON stdout is parsed by `dlm_ytdlp_parse` — see
[architecture.md](architecture.md).
