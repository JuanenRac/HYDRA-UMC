# Build switches of the firmware that ship off

Every switch below is `0` in a build as it ships, so the binary does not change until a build sets it (`-DNAME=1`), except `HYDRA_FDCAN_KERNEL_CLOCK_EXPLICIT`, which the maintainer decided to ship on (`1`; build with `-DHYDRA_FDCAN_KERNEL_CLOCK_EXPLICIT=0` for the old behaviour). That one is not validated on a board: row 10 is still open. Each one has a host test that runs it on and off, and a row of `docs/BENCH_TESTS.md` that has to be run on a board before it is turned on in a release.

| Switch | File | What it does when on | Bench row to pass first |
|---|---|---|---|
| `HYDRA_FDCAN_KERNEL_CLOCK_EXPLICIT` | `src/mcu_stm32g474/fdcan_clock.h`, both bootloader and application | selects the APB1 clock (the HSI at divider 1, 16 MHz) as the FDCAN kernel clock; the selector resets to the HSE, which is not running | 10 |
| `HYDRA_APP_ENTER_BOOTLOADER` | `src/mcu_stm32g474/app_boot_request.h` | the application answers `ENTER_BOOTLOADER` (exact magic, only with the drivers off), leaves a request in a backup register and resets; the bootloader stays in its update window once | 14 |
| `HYDRA_HEALTH_FRAME` | `src/mcu_stm32g474/health_frame.h` | the application sends one 8-byte health frame per second on `slot_base + 0x11` | 19 |
| `HYDRA_READBACK_GATE` | `src/mcu_stm32g474/boot/adoption_guard.h` | the bootloader does not serve the read-back of the application while the read-out protection is on, and reads the metadata back after the first-boot adoption | 16 |
| `HYDRA_BOOT_REPORT` | `src/mcu_stm32g474/boot_report.h`, bootloader and application | the bootloader leaves a watchdog-reset flag and a failed-set-up flag in two backup registers before it jumps; the application puts them into the flags of the health frame (`WDG_RST`, `INIT_BAD`) | 19 |
| `HYDRA_INIT_FAULT_LATCH` | `src/mcu_stm32g474/init_fault.h`, application | a failed set-up call of the application writes a counter into two backup registers and resets once; a second failure leaves the drivers off with the interrupts off and the watchdog refreshed in a loop; a start after a failed one reports `INIT_BAD` in the health frame | 19 |
| `HYDRA_FLASH_LAYER` | `src/mcu_stm32g474/boot/bootloader_flash.c` (G474 first; the H745 cores follow) | the erase, write and copy routines of the bootloader go through `flash_layer.h`: range, alignment, blank-before-write checks and a read-back of every double-word, with the three hardware calls left to the chip | 22 to 25 |
| `HYDRA_QUIET_TICK_BEFORE_SCHEDULER` | `src/mcu_stm32g474/STM32G474RE_main.c` | `1`: the tick interrupt is off from the end of the set-up until the scheduler starts; `2`: off right after `HAL_Init()` | 12 |
| `HYDRA_SPI_SYSTEM`, `HYDRA_SPI_STOP_COMMAND` | `src/mcu_stm32h745/CM4/spi_system.h` | the telemetry and probe, and then the stop and release, commands of the SPI link (`docs/SPI_SYSTEM.md`) | 13, 19 |
| `HYDRA_RCC_HSEM_HANDSHAKE` | `src/mcu_stm32h745/CM4/rcc_owner.h` | one core of the H745 sets the clock tree up, chosen with a hardware semaphore; the other waits a bounded time or leaves it | 8 |

`src/*/boot/flash_layer.h` (the same file in the three boot directories) is the common erase, program and read-back layer; the G474 bootloader calls it behind `HYDRA_FLASH_LAYER` (off); the two H745 bootloaders do not call it yet. It is switched in chip by chip after rows 22 to 25 pass.

The time models behind rows 12, 13 and 19 are in `tests/sim_timing_models.py` with their numbers in `tests/timing_models.json`; they are assumptions to be replaced by the measurements, not measurements.
