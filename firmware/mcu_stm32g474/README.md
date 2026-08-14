# Robot Controller Board Firmware — STM32G474RET6

**Project:** HYDRA-UMC
**Author:** JuanenRac (Electro Hobby 3D) — electrohobby3d@gmail.com
**License:** GPL-3.0 — see repo root LICENSE

This is the firmware for the **Robot Controller Board** (Tier 1 of
`docs/architecture.md`): one per robot, up to 8 per HYDRA-UMC controller,
reached over FDCAN1 "STACK A" from the STM32H745 "Kinematic Brain", and
itself relaying one hop further to that robot's own URTC Tool Head.

## Status: real bootloader, application still a skeleton

Everything here compiles and links today (`../../build_firmware.sh g474`,
verified against a full `--clean` rebuild).

**`boot/` is a real CAN-OTA bootloader**, not a placeholder — bare-metal, no
FreeRTOS (a bootloader doesn't need a scheduler — see `docs/architecture.md`
section 2), ported from URTC's own proven bootloader (sibling repo) and
re-based onto this board's own FDCAN1 slot addressing
(`docs/architecture.md` section 4, `docs/PINOUT_STM32G474_ROBOT_
CONTROLLER.TXT` section 1a/1c): FDCAN1 listen loop, `SLOT_ID[2:0]`-derived
addressing, CRC32 + HMAC-SHA256 verify-into-backup-before-copy-to-main
anti-bricking discipline, version query, error counters, anti-rollback with
explicit downgrade authorization, backup readback. NOT yet verified against
real hardware — see `boot/bootloader_main.c`'s own header for the FDCAN
bit-timing assumption (16MHz HSI-derived) that needs revisiting once a real
clock tree exists.

`STM32G474RE_main.c` (the application, not the bootloader) is still a
**FreeRTOS** GPIO-toggle smoke test (one task, `xTaskCreate`) proving the
toolchain/HAL/linker/RTOS pipeline itself works — not real motion firmware
yet. It now blinks a real pin (`PC13`, `STATUS_LED` per the pinout doc)
instead of a Nucleo-dev-board placeholder, but doesn't yet initialize any
of this board's other real peripherals (the 6x TMC5160A SPI4 daisy-chain,
STEP/DIR/EN, 6x endstops, FDCAN2 to the URTC head) — those are pinned out
in `docs/PINOUT_STM32G474_ROBOT_CONTROLLER.TXT` but not yet wired up here.
See each file's own header comment for exactly what's still TODO, all
tracked against `../../docs/architecture.md`.

**FreeRTOS:** `FreeRTOSConfig.h` in this folder configures the kernel (16
KB heap, 1000 Hz tick — see that file's own header for why
`configCPU_CLOCK_HZ` is still the HSI reset default and needs updating
alongside real clock config). Vendored the same way as the HAL/CMSIS
sources — see `../../docs/COMPILE_STM32G474.TXT` section 3a.

## Hardware platform

| | |
|---|---|
| MCU | STM32G474RET6, LQFP-64 |
| Core | ARM Cortex-M4F, up to 170 MHz |
| Flash | 512 KB |
| RAM | 128 KB (80 KB SRAM1 + 16 KB SRAM2, contiguous, both mapped as one region here) + 32 KB CCM SRAM at `0x10000000` (unused/reserved) |
| CAN | 3x FDCAN peripherals — 2 used: one uplink to the STM32H745's own FDCAN1 (Tier 1), one downlink to this robot's own URTC Tool Head (Tier 2 relay) |
| Real job (not yet implemented) | 6-axis STEP/DIR/ENABLE generation, endstop reading, CAN-OTA bootloader (own firmware + relay to URTC head), FDCAN slot-addressed protocol from `docs/architecture.md` section 3 |

### Flash layout (512 KB total — see `STM32G474RETx_APP.ld`'s own header for exact addresses)

```
0x08000000 ─┬─ Bootloader        32 KB
0x08008000 ─┼─ Metadata           4 KB
0x08009000 ─┼─ App main slot    236 KB
0x08044000 ─┼─ App backup slot  236 KB
0x0807F000 ─┴─ Reserved           4 KB   (end of flash, 512 KB total)
```

PROPOSED — modeled directly on URTC's own proven bootloader/app/backup
split (see the sibling `URTC` repo), scaled up for this chip's larger flash.
Not yet verified against real hardware.

## Building

```bash
../../build_firmware.sh g474
```

See `../../docs/COMPILE_STM32G474.TXT` for the full compile reference (every
command this script runs, and why).

## Source layout

| File | Purpose |
|---|---|
| `STM32G474RE_main.c` | Application entry point — currently a FreeRTOS GPIO-toggle smoke test only |
| `STM32G474RETx_APP.ld` | Application linker script (main flash slot) |
| `FreeRTOSConfig.h` | FreeRTOS kernel config for this board's application (bootloader never includes this) |
| `boot/bootloader_main.c` | Bootloader entry point (bare-metal) — clock config, FDCAN1 + SLOT_ID init, real CAN-OTA main loop |
| `boot/bootloader_common.h` | Shared types/defines: flash layout, FDCAN1 offset table, HMAC key, status codes |
| `boot/bootloader_crypto.c/h` | SHA-256 / HMAC-SHA256 (ported verbatim from URTC's own, chip-independent) |
| `boot/bootloader_flash.c/h` | CRC32, flash program/erase (double-word), metadata persistence |
| `boot/bootloader_protocol.c/h` | FDCAN protocol handlers, app validation, jump-to-application |
| `boot/STM32G474RETx_BOOTLOADER.ld` | Bootloader linker script |

`stm32g4xx_hal_conf.h` and every HAL/CMSIS header are NOT vendored here —
`build_firmware.sh` fetches them fresh from ST's own official repos each
run (cached under `../../build/g474/vendor/`). See
`../../docs/COMPILE_STM32G474.TXT` section 3.
