# ipc_driver — CM5 ↔ STM32H745 SPI link (superseded by `../spi_bridge/`)

**Project:** HYDRA-UMC
**Status:** 🚧 skeleton only, superseded — the real, working implementation
of everything this C skeleton was meant to become now lives in
[`../spi_bridge/`](../spi_bridge/README.md) (Python, real 128-byte framing,
real bootloader state machine, unit-tested). This C skeleton is kept, not
deleted, in case a future C/embedded-Linux implementation is preferred
over the Python one — see `../spi_bridge/README.md` for why Python was
chosen (reuse of the sibling `URTC-FLASHER` tool's own proven CRC32/HMAC
state-machine logic, rather than re-deriving it in C). See
`src/ipc_driver.c`'s own header for what this skeleton itself implements
vs. TODO.

Linux-side counterpart to the STM32H745's own SPI1 slave-mode IPC
(`../../mcu_stm32h745/`, README.md section 10): a full-duplex SPI1 link,
designed for up to 50 MHz (this driver defaults to a conservative 10 MHz
today, until the STM32H745 SPI1 slave-side config it needs to match has
been verified against real hardware), with a `HYDRA_DATA_READY` handshake
GPIO the STM32 asserts when a 128-byte telemetry frame is ready in shared
AXI SRAM for the CM5 to fetch over SPI DMA.

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
- The Tier-0 SPI-OTA bootloader client (uploading firmware to the
  STM32H745 itself — `docs/architecture.md` section 2)

## Done since this skeleton was written

- `HYDRA_DATA_READY` GPIO edge-interrupt handling — `hydra_ipc_open()`/
  `hydra_ipc_read_frame()` now really request the line via libgpiod v2's
  edge-event API and block on a real rising edge (or a caller-supplied
  timeout) before the SPI transfer, instead of polling the bus
  unconditionally. Still unverified against real hardware (no STM32H745
  firmware exists yet to assert the line), but the handshake itself is
  real code now — see `src/ipc_driver.c`'s own header for the exact state.
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
