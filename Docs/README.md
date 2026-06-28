# dlm documentation

`dlm` is a lean download manager written in C11. It pairs a segmented HTTP(S)
engine (resume, checksum verification) with `yt-dlp` for streaming sites, and
ships three front-ends over one background daemon:

```
  dlm (CLI)        dlm-gui (GTK4 / libadwaita)
       \                 /
   JSON-lines over an AF_UNIX socket
                |
              dlmd  ── daemon: queue scheduler + IPC server
                |
            libdlm  ── engine (libcurl multi: segmented + resume),
                       SQLite queue store, extractors, checksum verify,
                       auto-managed external tools, platform compat layer
                         ├─ archive.org  (native: IA Metadata/Download API)
                         └─ yt-dlp       (subprocess: http → engine, HLS/DASH → delegate)
```

## Contents

| Document | Topic |
|----------|-------|
| [requirements.md](requirements.md) | Build- and run-time dependencies (Linux + Windows) |
| [building.md](building.md) | Building on Linux and on Windows (MSYS2 / MinGW-w64) |
| [architecture.md](architecture.md) | Components, data flow, on-disk locations |
| [protocol.md](protocol.md) | The daemon IPC protocol **and** the yt-dlp progress protocol |
| [auto-tools.md](auto-tools.md) | Auto-download & weekly update of yt-dlp / ffmpeg |
| [compat.md](compat.md) | The platform-compatibility layer and the Windows port |

## Quick start

```sh
./build.sh                 # configure + build + run tests
./build/dlm get <url>      # download directly (no daemon)
./build/dlm add <url>      # enqueue on the daemon (auto-started)
./build/dlm watch          # live status table
./build/dlm-gui            # GUI
```

External tools (`yt-dlp`, `ffmpeg`, `ffprobe`) are **fetched automatically** on
first use if they are not already installed — see [auto-tools.md](auto-tools.md).

## License

dlm is licensed **GPL-3.0-or-later** (GNU GPL v3 or any later version). The full
text is in [../LICENSE](../LICENSE), and every source file carries an SPDX
`GPL-3.0-or-later` identifier.
