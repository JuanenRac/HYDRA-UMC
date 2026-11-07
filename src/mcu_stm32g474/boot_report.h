/* =============================================================================
 * HYDRA-UMC - src/mcu_stm32g474/boot_report.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * What the bootloader found at power-up, handed to the application so that the health frame can say it (HAL-free, host-testable).
 *
 * The bootloader records two facts before it jumps to the application: the reset that brought the board here was a watchdog reset, and a set-up
 * call of its own (clock, FDCAN init, filter, start, SPI) failed. The application would otherwise never know. They travel in two backup registers
 * that survive the jump (BKP2R and BKP3R of the TAMP block, next to the two of app_boot_request.h): a word with its complement, so a register that
 * holds random bits after a power-up is not a report. The application reads the report once, clears it, and puts the facts into the flags of the
 * health frame (HEALTH_FLAG_WDG_RST, HEALTH_FLAG_INIT_BAD).
 *
 * Everything is behind HYDRA_BOOT_REPORT (default 0): with 0 the bootloader writes nothing and the application reads nothing.
 * Evidence of the tests: simulator (tests/test_app_support.c); the registers across a real jump are bench row 19.
 * ========================================================================== */
#ifndef HYDRA_UMC_BOOT_REPORT_H
#define HYDRA_UMC_BOOT_REPORT_H

#include <stdint.h>
#include "health_frame.h"

#ifndef HYDRA_BOOT_REPORT
#define HYDRA_BOOT_REPORT 0
#endif

#define BOOT_REPORT_MAGIC 0xB007E000u        /* the upper 24 bits of the first word; the lower 8 carry the facts */
#define BOOT_REPORT_MAGIC_MASK 0xFFFFFF00u
#define BOOT_REPORT_WATCHDOG_RESET 0x01u
#define BOOT_REPORT_INIT_FAILED    0x02u
#define BOOT_REPORT_KNOWN          (BOOT_REPORT_WATCHDOG_RESET | BOOT_REPORT_INIT_FAILED)

/* The bootloader, just before it jumps: `report` is &TAMP->BKP2R on the board (two consecutive registers) and a variable on the host. */
static inline void BootReport_Write(volatile uint32_t *report, uint8_t watchdog_reset, uint32_t hal_fault_bits)
{
#if !HYDRA_BOOT_REPORT
    (void)report; (void)watchdog_reset; (void)hal_fault_bits;
#else
    const uint32_t facts = (watchdog_reset ? BOOT_REPORT_WATCHDOG_RESET : 0u) | (hal_fault_bits != 0u ? BOOT_REPORT_INIT_FAILED : 0u);
    report[0] = BOOT_REPORT_MAGIC | facts;
    report[1] = ~report[0];
#endif
}

/* The application at the start: the health flags the report asks for (0 when there is no valid report), and the report is cleared. */
static inline uint8_t BootReport_TakeHealthFlags(volatile uint32_t *report)
{
#if !HYDRA_BOOT_REPORT
    (void)report;
    return 0u;
#else
    uint8_t flags = 0u;
    const uint32_t word = report[0];
    if ((word & BOOT_REPORT_MAGIC_MASK) == BOOT_REPORT_MAGIC && report[1] == ~word) {
        if (word & BOOT_REPORT_WATCHDOG_RESET) flags = (uint8_t)(flags | HEALTH_FLAG_WDG_RST);
        if (word & BOOT_REPORT_INIT_FAILED) flags = (uint8_t)(flags | HEALTH_FLAG_INIT_BAD);
    }
    report[0] = 0u;
    report[1] = 0u;
    return flags;
#endif
}

#endif /* HYDRA_UMC_BOOT_REPORT_H */
