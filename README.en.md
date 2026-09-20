# yt-popball-2

#### Description

popball2 is a Qt6 desktop **floating-ball system monitor widget + clipboard transfer station**,
always kept on top of the desktop:

- **System monitoring**: area charts for memory & swap usage in real time, CPU usage curve,
  CPU temperature, CPU frequency, up/down network speed and disk I/O speed — all values shown
  in a seven-segment LCD style, with configurable colors and per-metric visibility;
- **Multiple ball shapes**: the ball supports four shapes — sphere (default) / rounded
  rectangle / square / wide bar — each with its own LCD text size and placement (the wide bar
  lays text out in two rows); switch in Settings;
- **Edge sidebar**: drag the ball to the left/right edge of the screen and it snaps into a
  rounded vertical bar showing CPU / memory / swap usage as slim bar charts that stay out of
  your way;
- **Transfer station**: hover the cursor over the ball for a moment (or drop files onto it) and
  a panel pops up — clipboard text, images and files are collected automatically, with four
  view modes, hover preview, a small text editor, drag-in/drag-out, a "clipboard only /
  transfer only" source filter, and history persisted to local SQLite so nothing is lost
  across restarts.

Supported platforms: **macOS** (Apple Silicon / Intel), **Linux** (Xorg / Wayland — Debian/Ubuntu,
Fedora/RHEL, openSUSE, Arch, Alpine, Void, …) and **Windows**. The sensor layer adapts to
x86_64, ARM64, Apple Silicon, IBM POWER, IBM Z, and SoC naming variations such as Qualcomm
Snapdragon / MediaTek Dimensity.

#### Software Architecture

- Framework: Qt 6 (C++17); Qt Creator shipped with Qt6 works as the IDE
- Layers:
  - `widget` — the ball itself: drawing, dragging, edge sidebar, hover polling
  - `popdock` — the transfer-station panel: four view modes, type filter, hover preview
    (image / text / video), small text editor, clipboard watcher, slide in/out animations
  - `clipstore` — clipboard history (SQLite via QtSql/QSQLITE; gracefully degrades to
    non-persistent when the module is unavailable at build time)
  - `sysInfo` — cross-platform system info collection (CPU temp/freq/usage, memory, swap,
    network speed, disk I/O)
  - `config` — INI configuration read/write (defaults, migration, `POPBALL2_CONFIG` override)
  - `settingsdialog` — flat-style settings window
  - `macwindow` — macOS accessory behavior (hidden from Dock / Cmd+Tab, visible on all Spaces)
- Optional modules (detected by qmake; the app builds and runs without them):
  - `QtSql` → clipboard history persistence (Linux runtime additionally needs the QSQLITE plugin)
  - `QtMultimedia` → muted looping video playback in the hover preview (falls back to a static
    frame when absent)

#### Quick Start

```bash
./run.sh              # interactive: install dev deps -> build -> run
./run.sh -y           # accept all defaults
./run.sh --help       # all options
```

`run.sh` auto-detects the distro (Debian/Ubuntu, Fedora/RHEL, openSUSE, Arch, Alpine, Void, …)
or macOS (Homebrew) and installs whatever Qt6 dev packages are missing; it also interactively
asks for the build directory, install prefix and Qt location.

Manual build:

```bash
mkdir build && cd build
qmake6 ../popball2.pro && make -j$(nproc)
./popball2.app/Contents/MacOS/popball2     # macOS
./popball2                                 # Linux / Windows
```

#### Packaging

One-command packaging (all formats for the current platform, output to `dist/`):

```bash
./package.sh                 # Linux: deb+rpm+AppImage   macOS: dmg+zip
./package.sh appimage        # generic AppImage (via Docker on macOS; target arch = host arch)
./package.sh deb rpm         # only the given formats
./package.sh --version 1.2.0 # explicit version (defaults to VERSION in the .pro)
./package.sh --help
```

An npm entry point is also provided (Node wrapper forwarding to `pack.js`, same logic):

```bash
npm run deb:x64     # build Linux x86_64 .deb (Docker or local cross-compile)
npm run deb:arm     # build Linux arm64 .deb
npm run rpm:x64     # / rpm:arm / appimage:x64 / appimage:arm / mac / win
npm run all         # all formats for the current platform
```

Running `./package.sh` once on Linux yields deb / rpm / AppImage; once on macOS it yields
dmg / zip, and adding the `appimage` target produces an extra AppImage via Docker.
**The same script works on both platforms**: package once on Linux → once on macOS → all
formats done. (Cross-compilation across platforms is not supported; rpm needs `rpmbuild`;
AppImage uses the bundled linuxdeploy / appimagetool / runtime in `tools/`, downloading only
if missing. Note that AppImage can only be built when "target arch == host arch" —
linuxdeploy cannot cross-arch.)

| Target | Artifact | Notes |
|---|---|---|
| `deb` | `popball2_<ver>_<arch>.deb` | Debian / Ubuntu |
| `rpm` | `popball2-<ver>-1.<arch>.rpm` | Fedora / RHEL / openSUSE (needs `rpmbuild`) |
| `appimage` | `popball2-<ver>-<arch>.AppImage` | generic Linux, Qt bundled; native build on Linux host, Docker build on macOS host |
| `mac` | `popball2-<ver>-macos-<arch>.dmg` / `.zip` | Qt bundled via macdeployqt, standalone |

All artifacts go to `dist/`. macOS packages are ad-hoc signed; use your own developer
certificate for formal distribution.

#### Dependencies & "Install & Run"

| Format | Self-contained? | Key point |
|---|---|---|
| macOS dmg/zip | ✅ Qt bundled | Gatekeeper may block first launch, see below |
| Linux AppImage | ✅ Qt bundled | needs FUSE; without FUSE use `--appimage-extract` |
| Linux deb | ❌ uses system Qt6 | `sudo apt install ./xxx.deb` resolves Qt6 automatically |
| Linux rpm | ❌ uses system Qt6 | `sudo dnf install ./xxx.rpm` resolves Qt6 automatically |

> ⚠️ Do **not** install deb/rpm with bare `dpkg -i` / `rpm -ivh` — dependencies are not
> resolved that way; use the package-manager form (`apt install` / `dnf install` /
> `zypper install` with a local path). `./build.sh` builds real runnable Linux **x64 and arm**
> packages via Docker on macOS.

Full dependency list, install commands, per-distro notes, architecture & glibc requirements:
see **[DEPENDENCIES.md](DEPENDENCIES.md)**.

#### Usage

1. **The ball** stays on top of the desktop; drag it with the left button and the position is
   remembered (positioning is restricted under Wayland). The LCD rows (temperature / frequency /
   disk I/O / up-down speed) can be individually hidden.
2. **Edge sidebar**: drag the ball to the left/right screen edge to snap it into a rounded
   vertical bar showing CPU / memory / swap as slim bar charts (bar height = current percentage,
   pale track = full scale). The bar only moves vertically along the edge; hovering shows each
   metric's percentage. Drag the bar inward, or click it, to return to the ball.
   **Width / height / corner radius are all adjustable** (default 30×100, radius 8):
   panel top-right "⚙ Settings → Sidebar", with one-click presets "Narrow 20 / Standard 30 /
   Wide 36" or manual width/height/radius (applied immediately).
3. **Transfer station**: hover over the ball for ~300ms (or drop files onto it) and the panel
   slides out:
   - **Collect**: copied text/images come in automatically; drag files into the panel; use the
     "pencil+plus" button on the bottom-right to add a note manually; `Ctrl/Cmd+V` pastes the
     current clipboard.
   - **Four views**: switch between Icon grid / List / Detail / Preview on the bottom-left;
     "Detail" shows type, size, path and a text snippet per item.
   - **Type filter**: tabs below the title appear dynamically based on the station's
     actual content — "All" is always there, plus Docs / Images / Videos / Installers /
     Archives / Audio / Executables / Fonts / Databases / Design (no tab for an empty
     category; it appears as soon as the first such file is dropped in, and wraps to a
     second row when the first is full). Installers cover common formats across
     Android / iOS / Windows / macOS / Linux (apk / ipa / exe / dmg / deb…);
     Executables include extension-less executable files; Fonts, Databases and
     Design / CAD / 3D each have their own extension whitelists. Categories are
     mutually exclusive;
     filtering only changes what you see, data and order are untouched, and the counter
     shows "visible / total".
   - **Source filter**: two switches above the tabs — "Clipboard only" and "Transfer only"
     (mutually exclusive). "Clipboard only" shows only items pasted/copied from the clipboard,
     "Transfer only" shows only files dropped in or notes added. Both off (default) = show all.
     It composes orthogonally with the type tabs (type first, then source), affects only what
     you see, and legacy records without a source marker count as "clipboard".
   - **Preview**: hovering an item pops a preview (image at full size / text content / file icon /
     muted video playback); the caption line shows the full file name (wraps when long), with a
     detail bar at the bottom showing
     "type · dimensions · size · full path"; moving away closes it.
   - **Open & edit**: double-click an item — text opens in the small editor (auto-saves on
     close), everything else opens with the system default app; right-click an item for
     Copy / Open / Delete; `Delete` also removes.
   - **Drag out**: drag an item to the desktop/finder to copy out the file or image.
   - **Persistence**: everything is stored in the local clipboard-history DB (default
     `~/.popball2_clipboard.db`); after restart it is restored in most-recently-used order;
     identical content only bumps its rank, never duplicates.
   - **Theme sync**: the panel's selected highlight, context-menu tint, and self-drawn icons
     (text-item "T" icon, the "New note" / "Save note" buttons) follow the accent color
     (`main_border_color`) and refresh instantly on theme change.
4. **Dock operation buttons** (the row of buttons at the panel's top-right; the ball's
   old right-click menu was folded into the panel, so right-clicking the ball no longer
   opens a menu):
   - The "⚙ Settings" button directly opens the flat settings window (6 preset palettes,
     11 color entries, size/border/
     shadow length (0 = none)/opacity/refresh rates, per-metric toggles, disk picker, sidebar
     presets, shape mask…; Apply / Restore-defaults (with confirmation); changes take effect on save);
   - The "📈 System Monitor" button directly opens the system monitor (Activity Monitor on macOS,
     desktop-detected on
     Linux; a custom command can be configured under Settings → System Monitor; single instance);
   The three button icons use the user-provided SVG shapes (settings gear / performance-stat
   polyline / power switch); at render time the original gray `#515151` is replaced with the
   theme accent color (`main_border_color`), refreshing immediately on theme switch
   (requires the Qt SVG module, auto-enabled via `qtHaveModule(svg)`).
   - The "⏻ power" button hides the window and exits safely.
5. **Config**: `~/.popball2_config.ini` (override the path with the `POPBALL2_CONFIG`
   environment variable). Colors, size, refresh intervals, shown components, sidebar settings
   and the transfer-station default view all live here — see the table below.
6. When the desktop has no compositing, set `shape_mask=1` to fall back to a shape mask
   (circle for the ball, rounded rect for the bar) to avoid a black rectangle; `shape_mask=2`
   forces it off; `0` is auto-detect.
7. Metrics that cannot be read (e.g. CPU frequency on Apple Silicon) are hidden automatically —
   no fake data is shown.
8. On macOS the app is an "accessory": it never appears in the Dock or Cmd+Tab, stays visible
   across Spaces and above full-screen apps — just like a classic floating-ball utility.

#### Config File (~/.popball2_config.ini)

| Section | Key | Meaning |
|---|---|---|
| `[appearance]` | `width` `height` | ball diameter (default 100) |
| | `aside_width` `aside_height` | sidebar width & height (default 30×100) |
| | `opacity` | window opacity (default 0.91) |
| | `shadow_radius` / `shadow_color` | shadow length (0 = none) / color |
| | `shape` | form: 0=ball, 1=sidebar (normally switched by the app) |
| | `main_color` `main_border_color` `main_border_width` | ball bg / border color / border width |
| | `mem_color` `swap_color` | memory / swap area-chart colors |
| | `cpu_usage_color` `cpu_usage_width` | CPU curve color / line width |
| | `cpu_freq_color` `cpu_temp_color` `net_speed_color` `disk_io_color` | LCD text colors |
| | `text_color` | "Ball text" master entry (in Settings: changing it syncs all four LCD text colors above; editing an individual entry takes precedence) |
| | `charts_rows` | chart history points (default 32) |
| `[components_show]` | `cpu_temp_show` `cpu_freq_show` `net_speed_show` `disk_io_show` | per-metric visibility (1=on / 0=off) |
| `[disk]` | `disk_io_mode` / `disk_io_name` | which disk for I/O stats: 0=highest-IO disk (default), 1=fixed disk |
| `[aside]` | `snap_to_edge` | sidebar toggle (1=on / 0=off, always ball) |
| | `aside_edge` | snapped side: 10=left, 11=right (managed by the app) |
| | `corner_radius` | sidebar corner radius (default 8) |
| `[position]` | `x` `y` | ball position |
| `[ui]` | `ball_style` | ball shape: 0=sphere (default) 1=rounded rectangle 2=square 3=wide bar |
| | `dock_view_style` | transfer-station default view: 0=icon grid (default) 1=list 2=detail |
| | `dock_width` / `dock_height` | transfer-station panel size in px (default 330×452; shrinks automatically when the screen is too small) |
| | `dock_position` | preferred panel position: 0=auto (side with more space, default) 1=right of ball 2=left of ball 3=screen center |
| | `dock_density` | panel content density: 0=compact 1=standard (default) 2=spacious |
| | `dock_opacity` | panel background opacity (permille 0-1000, default 871 = 87.1%) |
| | `dock_remember_scroll` | remember last scroll position in panel: 1=remember 0=don't (default; opens at top) |
| `[timer]` | `update_data_interval` / `update_ui_interval` | data / UI refresh interval in ms (default 450) |
| `[window]` | `shape_mask` | shape mask: 0=auto (detect compositing on X11) 1=force on 2=off |
| `[system_monitor]` | `cmd` | custom system-monitor command; empty = auto-detect |
| `[meta]` | `config_version` | config schema version (migration only, leave alone) |

#### License

This project is released under the **GNU General Public License v2 (GPLv2)** — see the
`LICENSE` file at the repository root. Any redistribution or modified version must provide
the source code under the same license (GPLv2).

#### Contribution

1.  Fork the repository
2.  Create a Feat_xxx branch
3.  Commit your code (`./push.sh "message"` pushes to both Gitee and GitHub in one go)
4.  Create a Pull Request
