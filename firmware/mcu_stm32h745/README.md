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

## Status: real CAN-OTA/SPI-OTA bootloaders, application still a skeleton

Both cores' app/bootloader pairs compile and link today
(`../../build_firmware.sh h745`, verified against a full `--clean`
rebuild).

**Both `boot/` folders are real bootloaders now**, not placeholders —
bare-metal, no FreeRTOS (`docs/architecture.md` section 2), implementing
`docs/architecture.md` sections 3-5's CAN-OTA/SPI-OTA design:

- **`CM4/boot/`** is the gateway — the only core with an external bus (SPI1
  to the CM5, FDCAN1 to STACK A per README.md sections 6/10). It handles 3
  destinations from one SPI1 frame format (`SpiOtaFrame_t`,
  `bootloader_common.h`): its own Bank 2 self-update, relayed-onward to a
  STACK A slot (Robot Controller Board, and via THAT board's own relay
  tunnel, URTC tiers 2-3), or relayed to CM7 over the mailbox below.
- **`CM7/boot/`** has no bus of its own — it's reached only through the
  new `Common/ipc_mailbox.h` (2-HSEM-channel shared-SRAM4 mailbox), which
  CM4's own gateway drives. Same protocol state machine as every other
  bootloader in this project, just fed by a mailbox poll instead of a bus
  RX FIFO.

Both share the same CRC32 + HMAC-SHA256 verify-into-backup-before-copy-to-
main anti-bricking discipline as URTC's own bootloader (sibling repo) and
this repo's own G474 bootloader. NOT yet verified against real hardware —
**real dual-core bring-up (CM4 boot release, cache enable, real clock tree,
cross-core scheduler-start coordination) is still NOT done** — see
`../../docs/COMPILE_STM32H745.TXT` section 6 for the complete list of what
that still needs.

Each application `main.c` (not the bootloaders) is still a **FreeRTOS**
GPIO-toggle smoke test (one task per core, TWO INDEPENDENT FreeRTOS
instances — this is a dual-core AMP chip, no shared scheduler state)
proving the toolchain/HAL/linker/RTOS pipeline works for both cores — not
real motion/relay firmware yet. Each now blinks a real spare pin (`PG9` for
CM7, `PG10` for CM4 — see `docs/PINOUT_STM32H745_KINEMATIC_BRAIN.TXT`
section 9c) instead of a placeholder that collided with real hardware
(the previous `PB0`/`PE0` are now `X_DIR`/`PUMP1`).

**FreeRTOS:** each core has its own `FreeRTOSConfig.h` (different heap
size — 64 KB for CM7's own 512 KB AXI SRAM, 32 KB for CM4's own 288 KB
SRAM — see each file's own header for why `configCPU_CLOCK_HZ` is still the
HSI64 reset default and needs updating alongside real clock config).
Vendored the same way as the HAL/CMSIS sources, one core-specific port each
(CM7 = ARM_CM7/r0p1, CM4 = ARM_CM4F) — see
`../../docs/COMPILE_STM32H745.TXT` section 3a.

## Hardware platform

| | |
|---|---|
| MCU | STM32H745ZIT6, LQFP-144 |
| Flash | 2 MB total — 1 MB Bank 1 (CM7) + 1 MB Bank 2 (CM4) |
| RAM | 1 MB total — CM7 uses D1-domain AXI SRAM (512 KB), CM4 uses D2-domain SRAM1+2+3 (288 KB); D3-domain SRAM4 (64 KB) now used for the CM7↔CM4 IPC mailbox, `Common/ipc_mailbox.h` |
| Host link | SPI1, up to 50 MHz, `HYDRA_DATA_READY` handshake GPIO — README.md section 10 |
| Fieldbus | FDCAN1, up to 8 Robot Controller Board slots — README.md section 6 |

### Flash layout (per bank, 1 MB each — see each `*.ld`'s own header for exact addresses)

```
+0x00000 ─┬─ Bootloader (sector 0)  128 KB
+0x20000 ─┼─ Metadata   (sector 1)  128 KB
+0x40000 ─┼─ App main slot (sectors 2-4)  384 KB
+0xA0000 ─┴─ App backup slot (sectors 5-7) 384 KB   (end of bank, 1 MB total, 8 sectors)
```
CM7 = Bank 1 @ `0x08000000`; CM4 = Bank 2 @ `0x08100000` (same layout, +0x100000).

**CORRECTED during CAN-OTA bootloader implementation**: STM32H7 erases
flash in 128 KB SECTORS (`FLASH_TYPEERASE_SECTORS`), not 2 KB pages the way
this project's F3/G4 boards do — the previous byte-arbitrary 64K/4K/476K/
476K/4K split (copied from the page-based mental model) didn't respect
that at all. Metadata also moved to its own dedicated sector rather than
sharing one with the bootloader's own running code — erasing the sector a
core is actively executing from is a real hazard on this chip, not just
untidy (see `CM7/boot/bootloader_common.h`'s own `METADATA_ADDR` comment).
Programming granularity is also different: 256-bit (32-byte) FLASHWORDs,
not the G4's 64-bit double-words or F3's 16-bit half-words.

Modeled on the same bootloader/app/backup philosophy as URTC's own proven
split and this repo's own G474 scripts, adapted for this chip's real flash
controller. Not yet verified against real hardware.

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
| `CM7/STM32H745ZI_CM7_main.c` | CM7 application entry point — FreeRTOS GPIO-toggle smoke test only |
| `CM7/STM32H745ZITx_CM7_APP.ld` | CM7 application linker script (Bank 1 main slot, sectors 2-4) |
| `CM7/FreeRTOSConfig.h` | FreeRTOS kernel config for CM7's own application (bootloader never includes this) |
| `CM7/boot/bootloader_main.c` | CM7 bootloader (bare-metal) — mailbox polling main loop |
| `CM7/boot/bootloader_common.h` | Shared types/defines: flash layout (Bank 1, sector-aligned), OFS_* offset table, HMAC key |
| `CM7/boot/bootloader_crypto.c/h` | SHA-256 / HMAC-SHA256 (ported verbatim from URTC's own) |
| `CM7/boot/bootloader_flash.c/h` | CRC32, sector-aware erase, FLASHWORD program, metadata persistence |
| `CM7/boot/bootloader_protocol.c/h` | Mailbox-relayed protocol handlers, app validation, jump-to-application |
| `CM7/boot/STM32H745ZITx_CM7_BOOTLOADER.ld` | CM7 bootloader linker script (sector 0, 128 KB) |
| `CM4/STM32H745ZI_CM4_main.c` | CM4 application entry point — FreeRTOS GPIO-toggle smoke test only |
| `CM4/STM32H745ZITx_CM4_APP.ld` | CM4 application linker script (Bank 2 main slot, sectors 2-4) |
| `CM4/FreeRTOSConfig.h` | FreeRTOS kernel config for CM4's own application (bootloader never includes this) |
| `CM4/boot/bootloader_main.c` | CM4 bootloader (bare-metal) — SPI1 + FDCAN1 init, the 3-way gateway main loop |
| `CM4/boot/bootloader_common.h` | Shared types/defines incl. `SpiOtaFrame_t` (the SPI1 wire format) and STACK A slot-addressing constants |
| `CM4/boot/bootloader_crypto.c/h` | SHA-256 / HMAC-SHA256 (own key, separate trust domain from CM7/G474) |
| `CM4/boot/bootloader_flash.c/h` | CRC32, sector-aware erase, FLASHWORD program, metadata persistence (Bank 2) |
| `CM4/boot/bootloader_protocol.c/h` | Self-update handlers + `Relay_ToStackA`/`Relay_ToCM7` |
| `CM4/boot/STM32H745ZITx_CM4_BOOTLOADER.ld` | CM4 bootloader linker script (sector 0, 128 KB) |
| `Common/ipc_mailbox.h` | CM7↔CM4 shared-SRAM4 IPC mailbox (2 HSEM channels) — now implemented and used by both bootloaders, not just reserved |

HAL/CMSIS headers are NOT vendored here — `build_firmware.sh` fetches them
fresh from ST's own official repos each run (cached under
`../../build/h745/vendor/`). See `../../docs/COMPILE_STM32H745.TXT` section 3.
