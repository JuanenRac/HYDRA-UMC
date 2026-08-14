# Kinematic Brain Firmware — STM32H745ZIT6 (dual-core)

**Project:** HYDRA-UMC
**Author:** JuanenRac (Electro Hobby 3D) — electrohobby3d@gmail.com
**License:** GPL-3.0 — see repo root LICENSE

This is the firmware for the **Kinematic Brain** (Tier 0 of
`docs/architecture.md`): the STM32H745ZIT6 co-processor wired directly to
the Compute Module 5 over SPI1, driving up to 8 Robot Controller Boards over
FDCAN1 "STACK A". Two independent cores, two independent firmware images:

- **`CM7/`** — Cortex-M7 @ up to 480 MHz. Real job (README.md section 5):
  S-curve motion engine, hardware timer pulse generation, the 6-axis local
  stage.
- **`CM4/`** — Cortex-M4 @ up to 240 MHz. Real job: FDCAN1 protocol
  management, analog sensor filtering, safety interlocks, inter-core IPC.

## Status: skeleton, not yet the real firmware

Both cores' app/bootloader pairs compile and link today
(`../../build_firmware.sh h745`, verified against a full `--clean`
rebuild), but each `main.c` is a GPIO-toggle smoke test proving the
toolchain/HAL/linker pipeline works for both cores — not real firmware.
**Real dual-core bring-up (CM4 boot release, HSEM sync, cache enable, real
clock tree, actual SPI1 IPC to the CM5) is NOT done** — see
`../../docs/COMPILE_STM32H745.TXT` section 6 for the complete list of what
that still needs, before assuming this skeleton is close to running on real
silicon.

## Hardware platform

| | |
|---|---|
| MCU | STM32H745ZIT6, LQFP-144 |
| Flash | 2 MB total — 1 MB Bank 1 (CM7) + 1 MB Bank 2 (CM4) |
| RAM | 1 MB total — CM7 uses D1-domain AXI SRAM (512 KB), CM4 uses D2-domain SRAM1+2+3 (288 KB); D3-domain SRAM4 (64 KB) reserved, unused today |
| Host link | SPI1, up to 50 MHz, `HYDRA_DATA_READY` handshake GPIO — README.md section 10 |
| Fieldbus | FDCAN1, up to 8 Robot Controller Board slots — README.md section 6 |

### Flash layout (per bank, 1 MB each — see each `*.ld`'s own header for exact addresses)

```
+0x00000 ─┬─ Bootloader        64 KB
+0x10000 ─┼─ Metadata           4 KB
+0x11000 ─┼─ App main slot    476 KB
+0x88000 ─┼─ App backup slot  476 KB
+0xFF000 ─┴─ Reserved           4 KB   (end of bank, 1 MB total)
```
CM7 = Bank 1 @ `0x08000000`; CM4 = Bank 2 @ `0x08100000` (same layout, +0x100000).

PROPOSED — modeled on the same bootloader/app/backup philosophy as URTC's
own proven split and this repo's own G474 scripts. Not yet verified against
real hardware.

## Building

```bash
../../build_firmware.sh h745
```

See `../../docs/COMPILE_STM32H745.TXT` for the full compile reference,
including the dual-core-specific detail (per-core compiler flags, which of
the 4 `system_stm32h7xx*.c` boot-strategy variants was picked and why).

## Source layout

| Path | Purpose |
|---|---|
| `CM7/STM32H745ZI_CM7_main.c` | CM7 application entry point — GPIO-toggle smoke test only |
| `CM7/STM32H745ZITx_CM7_APP.ld` | CM7 application linker script (Bank 1 main slot) |
| `CM7/boot/bootloader_main.c` | CM7 bootloader — jumps straight to the app unconditionally |
| `CM7/boot/STM32H745ZITx_CM7_BOOTLOADER.ld` | CM7 bootloader linker script |
| `CM4/STM32H745ZI_CM4_main.c` | CM4 application entry point — GPIO-toggle smoke test only |
| `CM4/STM32H745ZITx_CM4_APP.ld` | CM4 application linker script (Bank 2 main slot) |
| `CM4/boot/bootloader_main.c` | CM4 bootloader — jumps straight to the app unconditionally |
| `CM4/boot/STM32H745ZITx_CM4_BOOTLOADER.ld` | CM4 bootloader linker script |
| `Common/` | Reserved for shared memory structure definitions (CM7↔CM4 IPC mailbox) once that's designed — empty today |

HAL/CMSIS headers are NOT vendored here — `build_firmware.sh` fetches them
fresh from ST's own official repos each run (cached under
`../../build/h745/vendor/`). See `../../docs/COMPILE_STM32H745.TXT` section 3.
