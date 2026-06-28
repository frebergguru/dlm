# Auto-managed external tools

`dlm` needs `yt-dlp` for streaming sites and `ffmpeg`/`ffprobe` for muxing. Rather
than require the user to install and maintain them, the library
(`libdlm/tools.c`, public API in `libdlm/include/dlm/tools.h`) downloads them on
demand and keeps `yt-dlp` up to date — while **never re-downloading a tool that
already exists**.

## Resolution order (check-before-download)

`dlm_tool_path(name)` resolves a tool in this order:

1. **Managed binary** in `<data>/dlm/tools/` (e.g. `yt-dlp`, `yt-dlp.exe`) — used
   if present and executable.
2. **`PATH`** — if the user already has the tool installed, its bare name is
   returned so the OS PATH search finds it.
3. The **managed path** (which a subsequent ensure may populate).

`dlm_tools_ensure_ready()` downloads a tool **only when neither a managed copy
nor a PATH copy is usable.** An existing install is never overwritten by the
ensure step — only the explicit weekly update replaces `yt-dlp`.

## Where binaries live

`<data>/dlm/tools/` where `<data>` is `$XDG_DATA_HOME`/`~/.local/share` on Unix
or `%LOCALAPPDATA%` on Windows. Per-OS names: `yt-dlp` vs `yt-dlp.exe`,
`ffmpeg`/`ffprobe` (+`.exe`). On Unix, downloaded binaries are `chmod 0755`.

## Acquisition

| Tool | Source | Method |
|------|--------|--------|
| `yt-dlp` | `github.com/yt-dlp/yt-dlp/releases/latest` (`yt-dlp`, `yt-dlp.exe`, `yt-dlp_macos`) | single-file download via the segmented engine; **SHA-256 verified** against the release `SHA2-256SUMS` |
| `ffmpeg` + `ffprobe` | `github.com/yt-dlp/FFmpeg-Builds` (`...win64-gpl.zip` / `...linux64-gpl.tar.xz`) | download archive, list members with `tar -tf`, extract the two `bin/` binaries with `tar -xf`, move into place |

Notes / honest caveats:

- **ffmpeg is an archive**, so extraction shells out to `tar` (universal on
  modern Windows 10+, Linux, macOS). If `tar` is missing or extraction fails,
  dlm logs one warning and **falls back to `ffmpeg` on `PATH`** — it never fails
  the feature.
- **macOS has no first-party yt-dlp ffmpeg build**; that path is not auto-fetched
  and falls back to PATH.
- yt-dlp's own `--ffmpeg-location` is passed to point it at the managed ffmpeg
  directory when one exists.

## Update policy

State lives in `<data>/dlm/tools/state.json`:

```json
{
  "auto": true,
  "ytdlp": {"version": "2026.06.09", "installed_at": 1782683977, "last_check": 1782683977}
}
```

- `dlm_tools_check_updates(force)` skips if `< 7 days` since `last_check` (unless
  `force`). Otherwise it queries the latest tag from the GitHub API, and if it
  differs from the installed version (or the managed binary is missing) it
  re-downloads + verifies + atomically swaps it in. `last_check` is always
  refreshed.
- **Verify-before-swap**: a failed download/verification keeps the old binary.
- The 7-day window uses wall-clock `time()`, not the monotonic clock.

## Ownership & concurrency

- The **daemon owns tools.** On startup it runs `ensure_ready` + `check_updates`
  on a **detached thread** (never on the poll loop), and re-triggers the
  weekly-gated check roughly hourly.
- `dlm get` (no daemon) calls `ensure_ready` **lazily**, only when a download
  actually needs yt-dlp.
- A cross-process lock (`<tools>/.lock`, `flock` on POSIX / exclusive file handle
  on Windows) ensures the daemon and a CLI run never download simultaneously.
- Downloads are atomic (`*.new` → verify → rename), so a half-written binary is
  never executed.

## Control & UX

- **Disable:** `DLM_NO_AUTO_TOOLS=1` (or `auto:false` in state) → resolve only,
  no network. Behaviour then matches the original PATH-only model.
- **GUI:** Settings has an "Auto-download & update yt-dlp / ffmpeg" switch, a
  "yt-dlp version" line, and an "Update now" button. These flow through the IPC
  `set` (`auto_tools`) and `tools_update` commands ([protocol.md](protocol.md)),
  since the daemon is the owner.
- **Offline / errors** are non-fatal: dlm warns and falls back to `PATH`; if
  neither a managed nor a PATH binary exists, behaviour is exactly as before
  (yt-dlp invocation fails with the familiar "is it installed?" message).

## Public API (`dlm/tools.h`)

```c
const char *dlm_tool_path(const char *name);    // "yt-dlp"/"ffmpeg"/"ffprobe"
const char *dlm_tool_ffmpeg_dir(void);          // for yt-dlp --ffmpeg-location
int  dlm_tools_ensure_ready(int allow_network); // download only what's missing
int  dlm_tools_check_updates(int force);        // weekly (or forced) yt-dlp update
int  dlm_tools_auto_enabled(void);
void dlm_tools_set_auto_enabled(int on);
int  dlm_tools_status_json(char **out);         // status object for the GUI
```
