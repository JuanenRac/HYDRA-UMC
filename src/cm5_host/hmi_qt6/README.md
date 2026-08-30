# hmi_qt6 — Touch display shell

**Project:** HYDRA-UMC
**Status:** 🚧 Real kiosk lockdown + splash + retry-until-loaded logic
written against Qt6's real, documented API — but still genuinely
**unverified by actual compilation**: no Qt6 (and specifically no
`WebEngineWidgets`, the Chromium-based module this app needs) is
installed on this development machine, and a real Qt6 + WebEngine
install is multi-gigabyte — judged too large/slow to fetch just to
verify this one app in this session. Verify on target or a real Qt6 dev
box (`qt6-base-dev` + `qt6-webengine-dev` on Debian/Raspberry Pi OS)
before trusting any of this builds clean.

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
  `main.cpp` + `kiosk_view.{h,cpp}`. Not yet verified to actually
  configure/build (no Qt6 installed on this development machine) — see
  Status above.
- `src/main.cpp` — entry point: builds the splash screen, constructs
  `KioskView`, wires the splash to close only once the dashboard has
  genuinely finished loading.
- `src/kiosk_view.{h,cpp}` — the real lockdown/retry logic (new): a
  `QWebEngineView` subclass overriding `closeEvent`/`keyPressEvent`/
  `mouseMoveEvent`/`mousePressEvent`, plus `loadWithRetry()` for the
  cold-boot retry loop. See that header's own comment for the exact real
  Qt6 API each piece is built on.

## What's still needed

- **Actual compilation verification** against a real Qt6 + WebEngine
  install (see Status above) — the single highest-priority next step for
  this folder, ahead of any further feature work here.
- Wiring to whatever `os/` decides for how services start/supervise each
  other on boot (see `../../../os/README.md` — that decision is no
  longer open at the ecosystem level, HYDRA-UMC-OS already builds on
  Raspberry Pi OS; this folder's own systemd wiring to it is still undone).
- A real branded splash image (`buildSplashPixmap()` in `main.cpp` draws
  one in code today - a real asset would replace it, not require new
  splash-screen logic).
- OS-level brightness control and watchdog/health monitoring - neither
  has a real Qt6 API surface sketched here yet.
