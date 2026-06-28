# Requirements

## Build-time

| Component | Purpose |
|-----------|---------|
| C11 compiler (gcc/clang, or MinGW-w64 gcc on Windows) | — |
| CMake ≥ 3.16 | build system |
| pkg-config | dependency discovery |
| libcurl | HTTP(S) engine (curl multi for segmented downloads) |
| jansson | JSON parsing / generation |
| sqlite3 | persistent queue store |
| libpcre2-8 | regex |
| libcrypto (OpenSSL) | MD5 / SHA-1 / SHA-256 verification |
| pthreads | worker threads (winpthreads on Windows) |
| gtk4 + libadwaita-1 | GUI only (optional; `-DDLM_GUI=OFF` to skip) |

The GUI is auto-disabled if gtk4/libadwaita are not found. The CLI and daemon
have no GUI dependency.

## Run-time

| Tool | Needed for | Provided how |
|------|-----------|--------------|
| `yt-dlp` | extracting/downloading streaming sites (anything not archive.org) | **auto-downloaded** if missing, else taken from `PATH` |
| `ffmpeg` + `ffprobe` | muxing audio+video / HLS/DASH that yt-dlp must remux | **auto-downloaded** if missing, else taken from `PATH` |
| `tar` | unpacking the ffmpeg archive during auto-download | system (`/usr/bin/tar`, Windows 10+ `tar.exe`) |

`dlm` no longer requires the user to pre-install `yt-dlp`/`ffmpeg`: it manages
them under its data directory and keeps `yt-dlp` current. See
[auto-tools.md](auto-tools.md). Auto-management can be disabled with the
`DLM_NO_AUTO_TOOLS=1` environment variable or the GUI settings switch, in which
case the tools must be on `PATH`.

### Linux / distro packages (example, Debian/Ubuntu)

```sh
sudo apt install build-essential cmake pkg-config \
     libcurl4-openssl-dev libjansson-dev libsqlite3-dev \
     libpcre2-dev libssl-dev libgtk-4-dev libadwaita-1-dev
# yt-dlp / ffmpeg optional — dlm will fetch them if absent
```

### Windows (MSYS2 / MinGW-w64)

In the **MSYS2 MinGW64** shell:

```sh
pacman -S --needed \
  mingw-w64-x86_64-toolchain mingw-w64-x86_64-cmake \
  mingw-w64-x86_64-pkg-config \
  mingw-w64-x86_64-curl mingw-w64-x86_64-jansson \
  mingw-w64-x86_64-sqlite3 mingw-w64-x86_64-pcre2 \
  mingw-w64-x86_64-openssl \
  mingw-w64-x86_64-gtk4 mingw-w64-x86_64-libadwaita
```

Windows 10 version 1803 or newer is required for AF_UNIX socket support (used by
the daemon IPC). See [building.md](building.md) and [compat.md](compat.md).

## Environment variables

| Variable | Effect |
|----------|--------|
| `DLM_DOWNLOAD_DIR` | default output directory for the CLI |
| `DLM_DB` | override the queue database path |
| `DLM_LOG` | log level: `error`/`warn`/`info`/`debug` (default `info`) |
| `DLM_NO_AUTO_TOOLS` | `1` disables auto-download/update; tools must be on `PATH` |
| `XDG_*` (Linux) | standard base directories (see [architecture.md](architecture.md)) |
