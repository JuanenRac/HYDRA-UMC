# os — Compute Module 5 operating system image

**Project:** HYDRA-UMC
**Status:** 🚧 strategy + service-wiring scaffolding only — no actual image
build has been run, no base OS choice has been hardware-verified.

This is where the CM5's own OS image build lives — distinct from
`../src/cm5_host/`, which holds the individual *applications* (HMI
shell, AI inference, video streaming, SPI IPC driver) that run **on top of**
whatever this folder produces. Think of it as: `os/` builds and configures
the box; `src/cm5_host/` is what's installed inside it.

## Base OS choice — PROPOSED, not yet decided for real

Two realistic options, per README.md section 2 ("Raspberry Pi OS / Yocto
patched with `PREEMPT_RT`"):

1. **Raspberry Pi OS Lite (64-bit) + `PREEMPT_RT` patchset + first-boot
   provisioning script** — faster to get working, uses Raspberry Pi
   Foundation's own well-supported CM5 board support, easier for anyone
   else to reproduce a dev image. Downside: less control over exactly
   what's on the image, larger base footprint than a from-scratch build.
2. **Yocto (a custom `meta-hydra-umc` layer)** — full control, minimal
   reproducible image, the "real product" answer for something eventually
   shipped at any volume. Downside: real up-front investment (a working
   Yocto BSP layer for CM5 + Hailo-8 + Hailo-10 + PREEMPT_RT is itself a
   project, not a weekend), steeper for anyone else contributing to
   reproduce.

**Recommendation for right now (not yet acted on): start with option 1**
(Raspberry Pi OS + first-boot script) to get real hardware bring-up moving,
keep option 2 as the later "productionize" step once the software side
(cm5_host apps, STM32 firmware, PCBs) has actually been validated on real
hardware — validating on a slower-to-build-but-known-good base first,
rather than blocking hardware bring-up on a from-scratch OS build. This is
a recommendation to revisit explicitly, not a decision already made.

## Layout

- `systemd/` — unit files wiring `src/cm5_host/`'s own components
  together (start order, restart policy, dependencies). `hydra-hmi.service`
  is a starting example — references binaries/paths that don't exist yet
  (nothing in `cm5_host/` builds a real installable binary today), so this
  unit won't actually work until they do.
- `first_boot/` — provisioning script placeholder for whatever a freshly
  flashed SD card/eMMC needs done once (hostname, network config, growing
  the root partition, installing `cm5_host/`'s own services) — not written
  yet, just the folder and a note on what belongs here.

## What's still needed

- An actual decision on option 1 vs. 2 above (or a third option not listed)
- A real, tested boot chain: bootloader config, kernel/device-tree for CM5
  + this project's own custom hardware (the PCIe Gen3 switch fanning out to
  the Hailo-8 + Hailo-10 M.2 sockets, dual GL3523, STM32H745 SPI link) —
  none of the device-tree overlays this custom board will need exist yet
- Real systemd unit files once `cm5_host/`'s own components actually build
  and install somewhere
