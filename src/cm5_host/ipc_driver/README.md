# ipc_driver — CM5 ↔ STM32H745 SPI link

**Project:** HYDRA-UMC
**Status:** 🚧 skeleton only — see `src/ipc_driver.c`'s own header for what's implemented vs. TODO.

Linux-side counterpart to the STM32H745's own SPI1 slave-mode IPC
(`../../mcu_stm32h745/`, README.md section 10): a full-duplex SPI1 link, up
to 50 MHz, with a `HYDRA_DATA_READY` handshake GPIO the STM32 asserts when a
128-byte telemetry frame is ready in shared AXI SRAM for the CM5 to fetch
over SPI DMA.

This is also the transport HYDRA-UMC-STUDIO's own Flasher/Tester modules
will eventually use for real (currently running against a simulated
transport — see that repo's `src/lib/canOta.ts` and
`docs/architecture.md` section 2 for the Tier-0 SPI-OTA protocol this
driver needs to speak once real firmware exists on the STM32H745 side).

## What's here today

- `src/ipc_driver.c` / `.h` — userspace SPI + GPIO-interrupt skeleton
  (`spidev` + `libgpiod`), compiles standalone, does NOT yet implement the
  real 128-byte frame protocol or the Tier-0 bootloader command set — see
  the file's own header comment for the exact TODO list.

## What's still needed

- Real 128-byte telemetry frame parsing (format not finalized — needs to be
  defined alongside the STM32H745-side firmware that produces it, see
  `../../mcu_stm32h745/README.md`)
- `HYDRA_DATA_READY` GPIO edge-interrupt handling (libgpiod v2 API, matching
  whatever Raspberry Pi OS/kernel this project settles on — see `../../../os/README.md`)
- The Tier-0 SPI-OTA bootloader client (uploading firmware to the
  STM32H745 itself — `docs/architecture.md` section 2)
- A stable IPC boundary to `hmi_qt6/` and whatever eventually replaces
  HYDRA-UMC-STUDIO's own browser-based dashboard on real hardware (Unix
  domain socket, shared memory, or this driver exposed as a small local
  HTTP/WebSocket service — not decided yet)

## Building

No build system wired up yet (no CMakeLists.txt) — `src/ipc_driver.c`
depends on `linux/spi/spidev.h` and `gpiod.h` (libgpiod), both
Linux-specific; it will not compile on this Windows development machine.
Verify compilation directly on the target (or a cross-compile sysroot) once
real logic lands here.
