# os — Compute Module 5 operating system image

**Project:** HYDRA-UMC
**Status:** 🚧 strategy + service-wiring scaffolding only — no actual image
build has been run here, and the base OS choice below has already been
made for real in a sibling repo (see next section) — this folder predates
that repo and hasn't been reconciled with it yet.

This is where the CM5's own OS image build lives — distinct from
`../src/cm5_host/`, which holds the individual *applications* (HMI
shell, AI inference, video streaming, SPI IPC driver) that run **on top of**
whatever this folder produces. Think of it as: `os/` builds and configures
the box; `src/cm5_host/` is what's installed inside it.

## Base OS choice — already decided, for real, in HYDRA-UMC-OS

This folder's own text used to present "Raspberry Pi OS vs. Yocto" as
still an open decision. It isn't, any more: the separate repo
**[HYDRA-UMC-OS](https://github.com/JuanenRac/HYDRA-UMC-OS)** already
builds on **Raspberry Pi OS ARM64** for real — a real device agent
(`agent/`), real non-secret configuration (`config/`), a real hardened
`hydra-umc-agent.service` unit (`systemd/`), and a real, tested
provisioning flow (`provisioning/first_boot.sh`,
`install_cm5_base.sh`, `install_server.sh`, `install_voice_ui.sh`,
`install_splashscreen.sh`, `preflight_cm5.py` — proven idempotent by its
own test — `rollback.py`, a real backup/rollback mechanism). None of
that is hypothetical the way this folder's own placeholders are; see
that repo's own README/CHANGELOG for exactly what's real there today.

Yocto (a custom `meta-hydra-umc` layer) remains a real option for a later
"productionize" pass once the software side has been validated on real
hardware, but there is no work toward it anywhere in the ecosystem today
— Raspberry Pi OS is what every real provisioning script, install
script, and test in HYDRA-UMC-OS actually targets right now.

## What this means for this folder

`systemd/hydra-hmi.service` and `systemd/hydra-umc-studio.service` here
are still exactly what they always were — non-functional placeholders
referencing binaries/paths that don't exist yet — and now genuinely
duplicate ground HYDRA-UMC-OS's own `systemd/hydra-umc-agent.service`
and `provisioning/` already cover for real. Likewise `first_boot/` here
is still just a placeholder note, while HYDRA-UMC-OS's own
`provisioning/first_boot.sh` is real and tested. This folder has not
been reconciled with that repo yet — that reconciliation (most likely:
retiring this folder's own placeholders in favor of HYDRA-UMC-OS once
`src/cm5_host/`'s own components actually build real installable
binaries for its systemd units to reference) is real, undone work, not
silently glossed over here.

## Layout

- `systemd/` — placeholder unit files wiring `src/cm5_host/`'s own
  components together (start order, restart policy, dependencies).
  `hydra-hmi.service` is a starting example — references binaries/paths
  that don't exist yet (nothing in `cm5_host/` builds a real installable
  binary today), so this unit won't actually work until they do. See
  "What this means for this folder" above for how this now relates to
  HYDRA-UMC-OS's own real `systemd/`.
- `first_boot/` — provisioning script placeholder for whatever a freshly
  flashed SD card/eMMC needs done once (hostname, network config, growing
  the root partition, installing `cm5_host/`'s own services) — not written
  yet, just the folder and a note on what belongs here. HYDRA-UMC-OS's
  own `provisioning/first_boot.sh` already does the real, tested version
  of this for its own platform-layer packages.

## What's still needed

- Reconciling this folder with HYDRA-UMC-OS (see above) — the base OS
  decision itself is no longer open, only which of this folder's own
  placeholders still need to exist once that reconciliation happens.
- A real, tested boot chain: bootloader config, kernel/device-tree for CM5
  + this project's own custom hardware (the PCIe Gen3 switch fanning out to
  the Hailo-8 + Hailo-10 M.2 sockets, dual GL3523, STM32H745 SPI link) —
  none of the device-tree overlays this custom board will need exist yet,
  in this folder or in HYDRA-UMC-OS.
- Real systemd unit files once `cm5_host/`'s own components actually build
  and install somewhere.
