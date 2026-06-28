# Platform-compatibility layer & the Windows port

The engine, daemon and CLI were originally POSIX-only (fork/exec, AF_UNIX +
`poll`, pthreads, `pwrite`/`ftruncate`/`fsync`, XDG paths, termios). The Windows
port keeps that single codebase and routes the OS-specific calls through a thin
compat layer compiled **into `libdlm`**, so all three binaries get the shims with
no extra link edges. On POSIX the wrappers are near-zero-cost pass-throughs, so
Linux behaviour is unchanged.

Built with **MSYS2 / MinGW-w64 gcc** (native PE binaries), not MSVC.

## Modules (`libdlm/compat/`)

| Header | Provides |
|--------|----------|
| `compat.h` | platform detect (`DLM_WIN`/`DLM_POSIX`), `strcasecmp`→`_stricmp` macros, umbrella include |
| `dlm_paths.*` | per-user dirs (config/data/cache/runtime), socket & DB paths, `dlm_mkdir_p` |
| `dlm_sock.*` | `dlm_sock_t`, Winsock init, AF_UNIX listen/connect/accept, nonblocking, `dlm_poll`, read/write/close, `would_block`/`was_intr` |
| `dlm_proc.*` | `dlm_proc_spawn`(+stdout capture), `_read`(timeout), `_terminate`, `_wait`, `_spawn_detached` |
| `dlm_io.*` | `dlm_pwrite`, `dlm_ftruncate`, `dlm_fsync`, `dlm_rename_replace`, `DLM_O_BINARY` |
| `dlm_misc.*` | `dlm_sleep_ms`, `dlm_self_dir`, `dlm_prompt_hidden` (echo-off), `dlm_install_term_handler` |

## Key design decisions

### IPC — AF_UNIX on both OSes
Windows 10 1803+ Winsock supports `AF_UNIX SOCK_STREAM` (MinGW ships
`<afunix.h>`). Keeping AF_UNIX means one address model, one connect-probe
single-instance guard, and the existing `poll()` event loop is preserved
(`dlm_poll` → `poll`/`WSAPoll`). The real friction the shim absorbs is that a
Windows socket is an opaque `SOCKET`, not an `int` fd — so `read`/`write`/`close`
become `recv`/`send`/`closesocket`, and error checks go through
`dlm_sock_would_block()`/`_was_intr()` instead of `errno`.

> **Fallback (designed-in):** if AF_UNIX or `WSAPoll` misbehave on a given
> toolchain, the Windows backend of `dlm_sock` can switch to TCP loopback + a
> port file under `%LOCALAPPDATA%` without touching callers.

### Threads — keep pthreads
The queue uses pthreads verbatim; on Windows we link MSYS2 `winpthreads`. No
thread/mutex call sites changed.

### Processes — `dlm_proc`
`fork`/`exec`/`posix_spawn`/`waitpid`/`kill`/pipe become one API. Windows uses
`CreateProcess` (with MS-CRT argv→command-line quoting), `CreatePipe` for stdout
capture, `PeekNamedPipe` + timeout to keep the 200 ms cancel responsiveness in
`run_ytdlp`, `TerminateProcess`, and `WaitForSingleObject`+`GetExitCodeProcess`.
Detached daemon spawn uses `DETACHED_PROCESS | CREATE_NO_WINDOW`.

### File I/O — Windows gotchas handled
- **`DLM_O_BINARY`** is OR'd into every `open()` so text-mode translation never
  corrupts a download.
- **`dlm_rename_replace`** → `MoveFileEx(MOVEFILE_REPLACE_EXISTING)` because
  Windows `rename()` fails if the target exists. Used for the final
  `.dlmpart`→output rename, the journal, the IA credentials, and tool swaps.
- `dlm_pwrite` uses `WriteFile` with an `OVERLAPPED` offset (the engine writes
  from one worker thread per download, so this is race-free); `dlm_ftruncate`→
  `_chsize_s`, `dlm_fsync`→`_commit`.

### Paths, console, signals
Per-user directories come from `%APPDATA%`/`%LOCALAPPDATA%` on Windows and
`$XDG_*` on Linux. Hidden password entry uses `SetConsoleMode`(echo-off) vs
termios. Termination uses `SetConsoleCtrlHandler` vs `sigaction` (SIGPIPE is
POSIX-only and simply dropped on Windows).

### GUI
`g_unix_fd_add` is POSIX-only, so the GUI watches its subscribe socket through a
`GIOChannel` — `g_io_channel_win32_new_socket` on Windows,
`g_io_channel_unix_new` on Linux — and reads/writes it via `dlm_sock_*`.

## Per-OS behaviour differences (intentional)

- The IA credentials file is `0600` on POSIX; on Windows it relies on the
  per-user `%APPDATA%` location (no chmod equivalent).
- The `/tmp/dlm-<uid>.sock` socket fallback is POSIX-only; Windows always uses
  the `%LOCALAPPDATA%` path.

## Status / validation

- The Linux build and full test suite are green; the compat layer's POSIX paths
  are behaviour-preserving.
- **Windows code paths are compile-validated.** Every non-GUI translation unit
  (the five compat modules, `util.c`, `download.c`, `tools.c`, `iaauth.c`,
  `verify.c`, `httpget.c`, `ytdlp.c`, `dlmd.c`, `queue.c`, `ipc.c`, `client.c`,
  `dlm.c`) compiles cleanly with `x86_64-w64-mingw32-gcc` (GCC 16) at
  `-Wall -Wextra` against the real Windows headers (`winsock2.h`, `afunix.h`,
  `windows.h`, winpthreads).
- **Core platform layer is runtime-validated.** The compat objects link into a
  PE32+ `.exe` and, run under wine, exercise: `%LOCALAPPDATA%` path resolution,
  `AF_UNIX` `listen` + connect-probe (succeeds), and `dlm_proc_spawn` with
  stdout capture (`cmd /c echo` → exit 0).
- A cross-toolchain file (`cmake/toolchain-mingw64.cmake`) captures the working
  flags. Note: `__USE_MINGW_ANSI_STDIO=1` is required so `printf` honours
  C99 `%zu`/`%lld`, and the log format attribute uses MinGW's
  `__MINGW_PRINTF_FORMAT` archetype (`libdlm/internal.h`).
- **Not yet built here:** `gui/main.c` (needs the MinGW gtk4/libadwaita
  packages, which exist under MSYS2). Its Windows changes are confined to the
  GIOChannel socket-watch shim and the `dlm_sock_*` swaps. The `tests/` harness
  is POSIX-shaped; the shipped binaries do not depend on it.
