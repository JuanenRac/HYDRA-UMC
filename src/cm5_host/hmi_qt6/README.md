# hmi_qt6 — Touch display shell

**Project:** HYDRA-UMC
**Status:** ✅ Real kiosk lockdown + splash + retry-until-loaded logic,
verified by actual compilation on BOTH real targets now. Windows/MSVC:
Qt 6.7.3 (`win64_msvc2019_64`, via
[aqtinstall](https://github.com/miurahr/aqtinstall) — the same real,
official prebuilt Qt binaries the Qt Online Installer itself downloads,
just scriptable) with the real `WebEngineWidgets` module, `cmake -B build
-G "Visual Studio 16 2019" -A x64` + `cmake --build build --config
Release` — a real, unmodified `hydra_hmi.exe` came out the other end, no
source changes needed. **CM5/Linux (the real deployment target):** the
same unmodified `CMakeLists.txt`/`main.cpp`/`kiosk_view.{h,cpp}`, built
on a real Debian 13 (trixie) aarch64 CM5 against the real
`qt6-base-dev`/`qt6-webengine-dev` (6.8.2) + `cmake`/`build-essential`
Debian packages — `cmake -B build -DCMAKE_BUILD_TYPE=Release` +
`cmake --build build` produced a real ARM64 ELF `hydra_hmi` binary, no
source changes needed there either. Launching it (`QT_QPA_PLATFORM=offscreen`,
no real display attached over SSH) starts and initializes cleanly, then
reports it can't create a GL/Vulkan context - expected for a headless SSH
session with no real GPU/DRM access, not a code defect; running it against
the actual HDMI display with the CM5's own GPU driver stack is still real,
separate future work, same as picking this shell over the deployed
Chromium kiosk below.

**The real, deployed HDMI kiosk today is HYDRA-UMC-OS's own
`provisioning/install_kiosk.sh`** (minimal X11 + Chromium, verified on a
real CM5, see that repo's own CHANGELOG) - not this Qt6 shell. This folder
stays a documented future alternative, not something to install alongside
it: both would fight over the same tty1/display, and nobody has decided
this one replaces the Chromium kiosk yet. Verify it compiles and pick one
before ever installing both on the same device.

**Relationship to HYDRA-UMC-STUDIO:** the actual dashboard UI (robot jog
control, trajectories, cameras, modules, Flasher/Tester, etc.) is
**HYDRA-UMC-STUDIO** — a separate, already-substantial React + Vite + Node
web app (sibling repo). This folder is NOT a competing reimplementation of
that UI in Qt — it's the native shell around it once real hardware exists:
a Qt6 `QWebEngineView` kiosk wrapper pointed at HYDRA-UMC-STUDIO's own
locally-served dashboard (`npm start`, `http://localhost:3000` per that
repo's own README), providing what a browser alone doesn't:

- Boot splash screen before the web server/dashboard is ready — real now
  (`main.cpp`'s `QSplashScreen`, drawn in code rather than a checked-in
  image asset since none exists yet), kept on screen through however many
  retries `KioskView`'s own retry-until-loaded loop needs.
- Full-screen kiosk chrome (no browser UI, no accidental navigation away)
  — real now (`kiosk_view.cpp`): no right-click context menu
  (`setContextMenuPolicy(Qt::NoContextMenu)`), no accidental close
  (`closeEvent()` overridden to `ignore()` the window manager's own
  close/Alt+F4 delivery), Escape/F11 swallowed as defense-in-depth, cursor
  hidden after 5s idle (a touch panel has no persistent pointer need,
  reappears immediately on the next touch/mouse motion). A technician
  unlock combo (Ctrl+Alt+Shift+Q) is the one real way out.
- OS-level integration a web page can't do itself: brightness control,
  watchdog/health monitoring, first-boot provisioning UI (see `../../../os/`)
  — still future work, see below.

## Layout

- `CMakeLists.txt` — builds a Qt6 Widgets + WebEngineView app from
  `main.cpp` + `kiosk_view.{h,cpp}`. Verified to actually configure and
  build a real `hydra_hmi.exe`/`hydra_hmi` on both Windows/MSVC and a
  real CM5 (Debian 13 aarch64) — see Status above.
- `src/main.cpp` — entry point: builds the splash screen, constructs
  `KioskView`, wires the splash to close only once the dashboard has
  genuinely finished loading.
- `src/kiosk_view.{h,cpp}` — the real lockdown/retry logic (new): a
  `QWebEngineView` subclass overriding `closeEvent`/`keyPressEvent`/
  `mouseMoveEvent`/`mousePressEvent`, plus `loadWithRetry()` for the
  cold-boot retry loop. See that header's own comment for the exact real
  Qt6 API each piece is built on.

## What's still needed

- **A real on-screen run against the CM5's own HDMI output and GPU
  driver stack** — the on-device compile itself is now verified (see
  Status above); what's left is launching the built `hydra_hmi` with a
  real display attached (not `QT_QPA_PLATFORM=offscreen` over SSH) and
  confirming the kiosk lockdown/splash/retry behavior on real hardware,
  the same way `install_kiosk.sh`'s own Chromium kiosk already has.
- Wiring to whatever `os/` decides for how services start/supervise each
  other on boot (see `../../../os/README.md` — that decision is no
  longer open at the ecosystem level, HYDRA-UMC-OS already builds on
  Raspberry Pi OS; this folder's own systemd wiring to it is still undone).
- A real branded splash image (`buildSplashPixmap()` in `main.cpp` draws
  one in code today - a real asset would replace it, not require new
  splash-screen logic).
- OS-level brightness control and watchdog/health monitoring - neither
  has a real Qt6 API surface sketched here yet.
