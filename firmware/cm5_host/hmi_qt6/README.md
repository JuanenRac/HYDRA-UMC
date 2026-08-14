# hmi_qt6 — Touch display shell

**Project:** HYDRA-UMC
**Status:** 🚧 CMake + entry-point skeleton only, does not build/run
anything real yet.

**Relationship to HYDRA-UMC-STUDIO:** the actual dashboard UI (robot jog
control, trajectories, cameras, modules, Flasher/Tester, etc.) is
**HYDRA-UMC-STUDIO** — a separate, already-substantial React + Vite + Node
web app (sibling repo). This folder is NOT a competing reimplementation of
that UI in Qt — it's the native shell around it once real hardware exists:
a Qt6 `QWebEngineView` kiosk wrapper pointed at HYDRA-UMC-STUDIO's own
locally-served dashboard (`npm start`, `http://localhost:3000` per that
repo's own README), providing what a browser alone doesn't:

- Boot splash screen before the web server/dashboard is ready
- Full-screen kiosk chrome (no browser UI, no accidental navigation away)
- OS-level integration a web page can't do itself: brightness control,
  watchdog/health monitoring, first-boot provisioning UI (see `../../../os/`)

## Layout

- `CMakeLists.txt` — builds a Qt6 Widgets + WebEngineView app. Not yet
  verified to actually configure/build (no Qt6 installed on this
  development machine) — verify on target or a proper Qt6 dev environment
  before trusting this compiles.
- `src/main.cpp` — entry point: full-screen window, `QWebEngineView`
  pointed at `http://localhost:3000`, nothing else yet (no splash screen,
  no kiosk lockdown, no OS integration - all still TODO).

## What's still needed

- Splash screen while waiting for the dashboard's own Node server to come
  up (currently just points the web view at the URL immediately, will show
  a blank/error page for however long the server takes to start)
- Real kiosk lockdown (disable right-click context menu, disable
  Ctrl+anything, etc. — `QWebEngineView` defaults don't do this)
- Wiring to whatever `os/` decides for how services start/supervise each
  other on boot
