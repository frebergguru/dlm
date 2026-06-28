# Building

## Linux / Unix

```sh
./build.sh                     # clean configure + build + ctest
```

`build.sh` honours:

| Variable | Effect |
|----------|--------|
| `BUILD_DIR` | build directory (default `build`) |
| `BUILD_TYPE` | `Debug` (default) / `Release` |
| `NO_GUI=1` | skip the GTK4 GUI |
| `ASAN=1` | AddressSanitizer + UBSan build |
| `PREFIX` | install prefix |
| `NO_SYSTEMD=1` | skip the systemd user unit |
| `KEEP_BUILD=1` | don't wipe the build dir first |

Manual CMake:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build
```

Targets: `dlm` (CLI, output name `dlm`), `dlmd` (daemon), `dlm-gui` (optional),
and the `dlm` static library plus the per-test executables.

## Windows (MSYS2 / MinGW-w64)

The Windows port reuses the same C sources behind a small compat layer
([compat.md](compat.md)); it is built with the **MinGW-w64 gcc** toolchain, not
MSVC. From the **MSYS2 MinGW64** shell, after installing the packages in
[requirements.md](requirements.md):

```sh
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

This produces native `dlm.exe`, `dlmd.exe`, `dlm-gui.exe`. CMake adds, on
`WIN32`:

- `_WIN32_WINNT=0x0A00` (Windows 10 — AF_UNIX + `WSAPoll`)
- `__USE_MINGW_ANSI_STDIO=1` so `printf` honours C99 `%zu`/`%lld`
- links `ws2_32` (Winsock) and `shlwapi`
- `winpthreads` for the pthreads used by the queue/daemon

### Cross-compiling from Linux (optional)

A toolchain file is provided for building Windows binaries from a Linux host with
the MinGW-w64 cross-compiler (you still need MinGW builds of the dependencies on
`PKG_CONFIG_LIBDIR` for a full link):

```sh
cmake -S . -B build-win \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build-win -j
```

The non-GUI sources have been verified to compile under `x86_64-w64-mingw32-gcc`,
and the platform-compat layer additionally links and runs under wine (AF_UNIX
sockets + process spawn). See [compat.md](compat.md#status--validation).

### Packaging a portable Windows build

The `.exe`s depend on the MinGW runtime DLLs. For distribution, copy them next to
the executables (use `ntldd`/`ldd` on each `.exe` to enumerate), e.g. the GTK4 /
libadwaita / GLib / cairo / pango / gdk-pixbuf / curl / sqlite / jansson DLLs from
`/mingw64/bin`, plus the GTK support data:

- `share/glib-2.0/schemas` (compiled schemas)
- `lib/gdk-pixbuf-2.0`
- an icon theme (e.g. Adwaita) under `share/icons`

The CLI/daemon need only the smaller set (curl, sqlite, jansson, pcre2, crypto,
winpthread, libstdc++/gcc runtime). Tool binaries (`yt-dlp.exe`, `ffmpeg.exe`)
are fetched at run time into `%LOCALAPPDATA%\dlm\tools` — they need not be
bundled.

## Tests

`ctest` runs the offline unit/integration suite (util, store, queue, package,
ipc, ia, ytdlp-parsing). It does not hit the network. The auto-tools download
path is validated separately (it requires network access).
