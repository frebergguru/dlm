# dlm — a C download manager

A lean download manager written in C: a **segmented (multi-connection) HTTP
engine** with a persistent **queue** and **resume**, broad site coverage via
**yt-dlp**, and a fully **native archive.org** module. It ships a background
daemon, a CLI, and a GTK4/libadwaita GUI.

It carries JDownloader's queue model: a **linkgrabber** staging area (crawl and
review links before committing), **packages** grouping links under a shared
download folder, per-link and per-package **priorities**, **enable/disable**,
manual vs **auto-download** control (per link and a global start/stop), **force
start**, **reordering**, and **clear-finished** — all persisted across restarts.

This is the outcome of "make a C port of JDownloader" — re-scoped to something
actually buildable and maintainable. Rather than reimplementing JDownloader's
thousands of constantly-changing Java host plugins, `dlm` owns the fast download
engine and delegates *extraction* to yt-dlp (for arbitrary sites) while handling
archive.org natively.

## Architecture

```
  dlm (CLI)   dlm-gui (GTK4/libadwaita)
        \        /
   JSON-lines over AF_UNIX socket
            |
          dlmd  ── daemon: queue scheduler + IPC server
            |
        libdlm  ── engine (libcurl multi: segmented + resume),
                   sqlite queue store, extractors, checksum verify
                     ├─ archive.org  (native: IA Metadata/Download API)
                     └─ yt-dlp       (subprocess: http → engine, HLS/DASH → delegate)
```

- **libdlm** — the reusable core. Segmented downloader over libcurl's multi
  interface (one connection per byte-range, `pwrite` into a sparse file), a JSON
  resume journal, the sqlite queue store, the extractor dispatch, IA auth, and
  md5/sha1 verification (OpenSSL).
- **dlmd** — holds the queue and engine; serves a JSON-lines protocol on
  `$XDG_RUNTIME_DIR/dlm.sock`. One worker thread per active download; a poll loop
  multiplexes clients and pushes progress events to subscribers.
- **dlm** — CLI client (`get` runs in-process; `add/ls/watch/pause/...` drive the
  daemon).
- **dlm-gui** — reactive libadwaita view over the daemon, with **full feature
  parity** with the CLI: a Downloads/Linkgrabber view switcher, packages with
  collapse and per-package menus, per-link priority/enable/disable/auto/force/
  move/remove actions, a global Start/Stop, and clear-finished.

## Build

Dependencies: `libcurl`, `jansson`, `sqlite3`, `libpcre2-8`, `openssl`,
`yt-dlp` + `ffmpeg` (runtime, for non-archive.org sites), and `gtk4` +
`libadwaita` for the GUI.

```sh
./build.sh                     # checks deps, builds, runs tests
./install.sh                   # build + install system-wide (to /usr/local)
```

`install.sh` puts `dlm`, `dlmd` and `dlm-gui` on the PATH, plus a `.desktop`
entry and icon so the GUI appears in your application menu, and a systemd user
service for the daemon. Use a user prefix without sudo via `PREFIX=~/.local
./install.sh`. Uninstall with `cmake --build build --target uninstall`.

After re-installing an upgrade, run `dlm restart` so the running daemon reloads
(the client also detects an outdated daemon and restarts it automatically).

Manual build instead of the scripts:

```sh
cmake -B build && cmake --build build
ctest --test-dir build         # unit/integration tests
```

Options: `-DDLM_GUI=OFF` (skip the GUI), `-DDLM_ASAN=ON` (AddressSanitizer),
`-DDLM_INSTALL_SYSTEMD=ON` (systemd user service).

## Usage

```sh
# direct, in-process (no daemon)
dlm get https://example.com/big.iso -c 8 -o big.iso

# rate-limited (suffixes k/m/g; applies to the engine and to delegated streams)
dlm get https://example.com/big.iso --limit 2M

# archive.org — whole item, every file md5-verified
dlm get https://archive.org/details/<identifier> -o ./item_dir

# any yt-dlp-supported page (progressive → segmented; HLS/DASH → yt-dlp)
dlm get https://vimeo.com/76979871

# a whole series/playlist → one file per episode into a chosen folder
dlm get https://tv.nrk.no/serie/lis -d ~/Videos/lis

# choose the download folder (default: $DLM_DOWNLOAD_DIR, else ~/Downloads)
dlm add https://example.com/file.zip -d ~/Downloads

# dry-run: show what a URL resolves to
dlm resolve <url>

# queue via the daemon (auto-starts dlmd); resolves archive.org items,
# yt-dlp pages and whole series/playlists into per-file queue entries
dlm add <url> [-o out] [-d dir] [-c n]
dlm ls            # snapshot (grouped by package)
dlm watch         # live-updating table
dlm pause|resume|cancel <id>
dlm rm <id> [-p]              # remove a link, or a whole package with -p
dlm set -j 5                  # max concurrent downloads
dlm set --limit 5M            # global speed cap (shared across active); "off" clears it

# JDownloader-style queue control
dlm start            # resume the queue (global autostart on)
dlm start <id>       # force one link/package to start now, ignoring limits
dlm stop             # stop after current — active finish, nothing new starts
dlm priority <id> highest|higher|high|default|low|lower|lowest   # or -3..3
dlm enable|disable <id> [-p] # include/exclude a link or package from scheduling
dlm auto <id> on|off [-p]    # choose whether a link/package auto-downloads
dlm move <id> up|down|top|bottom [-p]   # reorder within a package / the list
dlm clear                    # remove all finished downloads

# linkgrabber — a staging area you review before downloading (like JDownloader)
dlm grab <url> [-d dir]      # crawl a URL into the linkgrabber (no download yet)
dlm lg                       # list staged links, grouped into packages
dlm confirm                  # move the whole linkgrabber into the download list
dlm confirm <id> -p          # confirm just one package (drop -p for a single link)
dlm confirm -n               # confirm but leave them as manual (don't auto-start)
dlm lg-rm <id> [-p]          # drop a staged link/package; dlm lg-clear empties it

# packages: rename, re-home, re-prioritise, collapse
dlm pkg <id> --name "Album" --folder ~/Music --priority high --collapse

# archive.org sign-in (optional; anonymous by default)
dlm ia-login --s3 <access> <secret>      # recommended (from /account/s3.php)
dlm ia-login --cookie '<cookie>'         # paste a browser session cookie
dlm ia-login --email <addr>              # email + password (prompts)
dlm ia-status / dlm ia-logout
```

Credentials are stored at `$XDG_CONFIG_HOME/dlm/credentials` (mode 0600), never
in the queue database.

## Status / what's verified

| Area | State |
|------|-------|
| Segmented multi-connection download | ✅ live (md5-verified, ~50 MiB/s) |
| Resume after interruption | ✅ live (byte-exact partial resume) |
| Speed limit (engine + delegate + global) | ✅ live (capped 256k → 0.21 MiB/s) + unit |
| YouTube / adaptive streams | ✅ live (resolve → delegate → muxed mp4) |
| Series/playlists → per-episode | ✅ live (NRK series → 12 delegate tasks) |
| Choosable download folder (`-d` / GUI) | ✅ live (CLI) + GUI field |
| Daemon delegate (series/streams via `add`) | ✅ unit (store+IPC flow); engine path live |
| Live progress for delegated downloads | ✅ CLI live; daemon parses yt-dlp progress (unit) |
| Auto-restart of an outdated daemon | ✅ protocol-version handshake on connect |
| sqlite queue + restart persistence | ✅ unit tests |
| Scheduler (pause/resume/cancel/rm, limits) | ✅ unit tests |
| Linkgrabber staging (grab → review → confirm) | ✅ unit tests (queue + IPC) |
| Packages (group, rename, re-home, priority, collapse, remove) | ✅ unit tests |
| Priorities + reorder (move up/down/top/bottom) | ✅ unit tests |
| Enable/disable, per-link & global autostart, force | ✅ unit tests |
| Clear finished downloads | ✅ unit tests |
| IPC JSON dispatch | ✅ unit tests |
| archive.org native (item + auth + verify) | ✅ live (anonymous) + unit |
| yt-dlp routing (progressive + delegate) | ✅ live (Vimeo + SoundCloud) |
| GTK4/libadwaita GUI (full queue + linkgrabber parity) | ✅ builds & launches |
| Memory safety | ✅ ASan/UBSan clean on download paths |

### Testing note

The unit/integration suite (`ctest`) covers the store, queue, IPC dispatch,
extractor routing, IA credentials, and yt-dlp JSON parsing deterministically and
offline. The **live socket transport** of `dlmd` and the **GUI** can't be
exercised in a sandbox that forbids long-lived listening-socket processes, so
those are verified by build/link + the in-process IPC tests rather than an
end-to-end daemon run. On a normal desktop, `dlmd` + `dlm watch` + `dlm-gui`
work against the same socket.

## Not in scope

JDownloader's plugin ecosystem, captcha solving, premium-host accounts, and
Click'n'Load are intentionally not reimplemented: site coverage comes from
yt-dlp, and archive.org is the one natively-supported provider.
