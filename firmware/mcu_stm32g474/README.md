# Robot Controller Board Firmware — STM32G474RET6

**Project:** HYDRA-UMC
**Author:** JuanenRac (Electro Hobby 3D) — electrohobby3d@gmail.com
**License:** GPL-3.0 — see repo root LICENSE

This is the firmware for the **Robot Controller Board** (Tier 1 of
`docs/architecture.md`): one per robot, up to 8 per HYDRA-UMC controller,
reached over FDCAN1 "STACK A" from the STM32H745 "Kinematic Brain", and
itself relaying one hop further to that robot's own URTC Tool Head.

## Status: skeleton, not yet the real firmware

Everything here compiles and links today (`../../build_firmware.sh g474`,
verified against a full `--clean` rebuild), but `STM32G474RE_main.c` is a
**FreeRTOS** GPIO-toggle smoke test (one task, `xTaskCreate`) proving the
toolchain/HAL/linker/RTOS pipeline itself works — not real motion/CAN-OTA
firmware. `boot/bootloader_main.c` stays **bare-metal, no FreeRTOS** (a
bootloader doesn't need a scheduler — see `docs/architecture.md` section 2)
and jumps straight to the application slot unconditionally, with no real
update protocol implemented. See each file's own header comment for exactly
what's still TODO, all tracked against `../../docs/architecture.md`.

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
| `boot/bootloader_main.c` | Bootloader entry point (bare-metal) — currently jumps straight to the app unconditionally |
| `boot/STM32G474RETx_BOOTLOADER.ld` | Bootloader linker script |

`stm32g4xx_hal_conf.h` and every HAL/CMSIS header are NOT vendored here —
`build_firmware.sh` fetches them fresh from ST's own official repos each
run (cached under `../../build/g474/vendor/`). See
`../../docs/COMPILE_STM32G474.TXT` section 3.
