/* =============================================================================
 * HYDRA-UMC - src/mcu_stm32g474/init_fault.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * What the application does when a set-up call of its own fails (HAL-free, host-testable).
 *
 * Until now a failed set-up call (clock selector, FDCAN init, filter, start) switched the drivers off, disabled the interrupts and spun: the watchdog
 * was not refreshed any more, so it reset the chip into the bootloader, and the bootloader's boot-attempt count decided whether the application was
 * tried again. This file adds the other behaviour, behind HYDRA_INIT_FAULT_LATCH (default 0: nothing changes):
 *
 *   - the first failure of a run of starts writes a counter into two backup registers (BKP4R and BKP5R of the TAMP block: a word with its
 *     complement, so random bits after a power-up are not a counter) and asks for ONE reset, because a transient fault is common;
 *   - a second failure, with the counter already at 1, asks for the safe state: the driver enables stay off, the interrupts stay off and the
 *     watchdog is refreshed in a loop, so the board neither resets in a circle nor moves, and the brain sees it fall silent;
 *   - a start whose set-up succeeds clears the counter, and a start that began with a counter above 0 reports HEALTH_FLAG_INIT_BAD in its health
 *     frame, so the host knows the board recovered from a failed set-up.
 *
 * Evidence of the tests: simulator (tests/test_app_support.c). The behaviour on a real board (a forced failure, the reset, the refreshed watchdog) is
 * bench row 19.
 * ========================================================================== */
#ifndef HYDRA_UMC_INIT_FAULT_H
#define HYDRA_UMC_INIT_FAULT_H

#include <stdint.h>
#include "health_frame.h"

#ifndef HYDRA_INIT_FAULT_LATCH
#define HYDRA_INIT_FAULT_LATCH 0
#endif

#define INIT_FAULT_MAGIC      0x1F17F000u   /* the upper 24 bits of the first word; the lower 8 carry the count */
#define INIT_FAULT_MAGIC_MASK 0xFFFFFF00u
#define INIT_FAULT_RESETS_ALLOWED 1u        /* one reset for a failed set-up, then the safe state */

typedef enum {
    INIT_FAULT_LEGACY_STOP = 0,   /* the switch is off: stop without refreshing the watchdog, as before */
    INIT_FAULT_RESET,             /* write the counter and reset the chip once */
    INIT_FAULT_SAFE_STATE         /* outputs off, interrupts off, watchdog refreshed for ever */
} InitFaultAction;

/* The number of failed starts recorded before this one (0 when there is no valid record or the switch is off). */
static inline uint8_t InitFault_Begin(volatile uint32_t *regs)
{
#if !HYDRA_INIT_FAULT_LATCH
    (void)regs;
    return 0u;
#else
    const uint32_t word = regs[0];
    if ((word & INIT_FAULT_MAGIC_MASK) == INIT_FAULT_MAGIC && regs[1] == ~word) return (uint8_t)(word & 0xFFu);
    return 0u;
#endif
}

/* A set-up call failed in a start that began with `prior` failed starts: what to do now (and, when a reset is asked for, the counter is written). */
static inline InitFaultAction InitFault_OnFailure(volatile uint32_t *regs, uint8_t prior)
{
#if !HYDRA_INIT_FAULT_LATCH
    (void)regs; (void)prior;
    return INIT_FAULT_LEGACY_STOP;
#else
    if (prior >= INIT_FAULT_RESETS_ALLOWED) return INIT_FAULT_SAFE_STATE;
    regs[0] = INIT_FAULT_MAGIC | (uint32_t)(prior + 1u);
    regs[1] = ~regs[0];
    return INIT_FAULT_RESET;
#endif
}

/* The set-up of this start finished: forget the failed starts. */
static inline void InitFault_Clear(volatile uint32_t *regs)
{
#if !HYDRA_INIT_FAULT_LATCH
    (void)regs;
#else
    regs[0] = 0u;
    regs[1] = 0u;
#endif
}

/* The health flag of a start that followed a failed one. */
static inline uint8_t InitFault_HealthFlags(uint8_t prior)
{
#if !HYDRA_INIT_FAULT_LATCH
    (void)prior;
    return 0u;
#else
    return prior > 0u ? (uint8_t)HEALTH_FLAG_INIT_BAD : 0u;
#endif
}

#endif /* HYDRA_UMC_INIT_FAULT_H */
