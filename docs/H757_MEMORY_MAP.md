# Memory map of the H757 board

The map is data: `docs/platform_profiles.json` (`memory_map` of each profile) holds every region with its owner, the document it comes from and its status, `tools/platform_profile.py` (`linker_memory`, `memory_problems`) generates the MEMORY block of a linker from it and checks it, and `tests/test_platform_profiles.py` compares the profile of today with the linker scripts of the repository. Nothing here has run on a board.

## Regions

| Region | Origin | Length | Owner | Where it comes from | Status |
|---|---|---|---|---|---|
| FLASH_BOOT_CM7 | `0x08000000` | 128 KB | CM7 | the linker scripts of the repository | inherited, to confirm for the H757 |
| FLASH_APP_CM7 | `0x08040000` | 384 KB | CM7 | the linker scripts of the repository | inherited, to confirm |
| FLASH_BOOT_CM4 | `0x08100000` | 128 KB | CM4 | the linker scripts of the repository | inherited, to confirm |
| FLASH_APP_CM4 | `0x08140000` | 384 KB | CM4 | the linker scripts of the repository | inherited, to confirm |
| RAM_CM7 (AXI SRAM, D1) | `0x24000000` | 512 KB | CM7 | the linker scripts of the repository | inherited, to confirm |
| RAM_CM4 (SRAM1-3, D2) | `0x30000000` | 288 KB | CM4 | the linker scripts of the repository | inherited, to confirm |
| SRAM4_MAILBOX (D3) | `0x38000000` | 64 KB | shared | `ipc_mailbox.h`; the only RAM both cores see, non-cacheable by the MPU | inherited, to confirm |
| SDRAM_BANK1 | `0xC0000000` | 128 MiB | CM7 | the nets document of the board, step 14 (two AS4C32M16SB-7BIN as 32 bits) and the FMC of the CubeMX project | from the board documents |
| SDRAM_BANK2 | `0xD0000000` | 128 MiB | CM7 | the same (two IS42S16320F-7TLI as 32 bits) | from the board documents |

The internal flash (2 MB in two banks) and the SRAM are those of the STM32H745/H747/H757 family: the H757 and the H745 are the same dual-core family with the same memory sizes, so the layout of the repository is kept. The reference manual (RM0399) is not in the folder of the board, only the datasheet of the H757XIH6, so the **status is "inherited, to confirm"**: before a linker of the H757 is used, check the flash banks and sectors, the SRAM sizes and the base addresses in RM0399 and in the datasheet.

The SDRAM is not usable for heap, framebuffer or DMA before the JEDEC start-up sequence, the real refresh and the data, address, marching and temperature tests the CubeMX guide asks for. The CubeMX values (CAS 3, 13 rows, 10 columns, SDCLK period 2, read burst, tMRD 2, tXSR 7, tRAS 5, tRC 7, tWR 2, tRP 3, tRCD 3) are starting values.

## Not memory mapped

The GD25Q128E NOR flash (16 MiB) is on **SPI5** in standard SPI mode (6.25 Mbit/s in the CubeMX project), written and read through the SPI5 driver. It has no region.

## Where the H757 board contradicts the H745 repository

* The repository has no external memory; the board has 256 MiB of SDR SDRAM on FMC that no linker or driver uses yet.
* The migration notes called the NOR flash QSPI; the board documents put it on SPI5, so no linker region and no QUADSPI driver.
* The CubeMX project gives every pin to the CM7; the repository gave SPI1, FDCAN1 and I2C4 to the CM4 and let the CM7 reach them through the mailbox. The owner decided the CM7 owns them, so the mailbox is rewritten in the phase of the profile (`docs/SPI_BOOT_MASTER.md`).
