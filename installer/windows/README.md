# Windows installers for dlm

Builds native Windows installers for **both 64-bit and 32-bit Windows**, bundling
the full GTK4 + libadwaita runtime so the GUI works on a clean machine. The
installer gives the user a Start Menu entry, an optional Desktop icon, and a
proper entry in **Apps & features** (Add/Remove Programs) for clean uninstall.

## Why Fedora rawhide

It is the one distro that ships prebuilt MinGW-w64 **GTK4** (and curl, jansson,
openssl, pcre2, sqlite, the Adwaita icon theme, and NSIS) for **both** the
`mingw32` and `mingw64` targets. MSYS2 dropped 32-bit; Debian/Ubuntu and openSUSE
don't package MinGW GTK4. The only missing piece is the GNOME **libadwaita**
library, which `prep-libadwaita.sh` compiles from source into both sysroots at
image-build time.

## Build

```sh
# from the repo root — builds the image if needed, then both installers
./build-win.sh

# force a fresh image, or stamp a version:
./build-win.sh --rebuild
VERSION=1.2.3 ./build-win.sh
```

Equivalent manual invocation:

```sh
docker build -t dlm-winbuild installer/windows
docker run --rm -v "$PWD":/src:ro -v "$PWD/dist":/out dlm-winbuild
```

Output in `dist/`:

| File | Target |
|------|--------|
| `dlm-<version>-setup-x64.exe` | 64-bit Windows (PE32+ x86-64) |
| `dlm-<version>-setup-x86.exe` | 32-bit Windows (PE32 i386) |

`VERSION` is taken from `git describe`; override with `-e VERSION=1.2.3`.
The libadwaita revision is pinned by `LIBADWAITA_TAG` (Docker build arg).

## What each installer contains

- `bin/` — `dlm.exe` (CLI), `dlmd.exe` (daemon), `dlm-gui.exe` (GUI) plus the
  complete DLL closure (GTK4, libadwaita, glib, cairo, pango, gdk-pixbuf, curl,
  openssl, sqlite, pcre2, …) gathered by walking the import tables.
- `lib/gdk-pixbuf-2.0/` — image loaders (incl. the SVG loader).
- `share/glib-2.0/schemas/gschemas.compiled` — GSettings schemas GTK needs.
- `share/icons/Adwaita` (+ `hicolor`) — the symbolic icons used across the UI.

GLib resolves these data directories relative to the executable, so no
environment variables are required at runtime.

## Files

- `Dockerfile` — the reproducible Fedora rawhide build environment.
- `prep-libadwaita.sh` — cross-compiles libadwaita into both sysroots (run once,
  baked into the image).
- `build.sh` — per-arch: cross-build the app, gather the runtime closure, run NSIS.
- `dlm.nsi` — the NSIS (Modern UI 2) installer: Program Files install, Start Menu
  entry, optional Desktop shortcut, uninstaller, and Add/Remove Programs registry.

## Verified

Each release of these scripts is smoke-tested under wine: silent install
(`/S`) lays down the tree, creates the Start Menu shortcut and the Apps &
features entry, `dlm-gui.exe --help` loads the entire GTK runtime with zero
missing DLLs, and the uninstaller removes all files, shortcuts and registry keys.
