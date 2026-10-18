/* =============================================================================
 * HYDRA-UMC Firmware - Host logic tests for BootDecision_ShouldJump
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * locks in the pure boot-attempt decision every real
 * bootloader_main.c (src/mcu_stm32g474/, src/mcu_stm32h745/CM4/,
 * src/mcu_stm32h745/CM7/) makes right before jumping to the application -
 * see boot_decision.h's own header comment for the full design. This exact
 * source file is compiled THREE times by build_firmware.sh's own host-tests
 * step, once per target's own include path, proving all three real copies
 * of boot_decision.h behave identically, not just one of them.
 */
#include <stdio.h>

#include "boot_decision.h"

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("  FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
            failures++; \
        } \
    } while (0)

int main(void)
{
    int failures = 0;
    printf("HYDRA-UMC host logic tests - boot_decision.h\n");

    /* A structurally invalid application is never jumped to, regardless of
     * boot_attempts - the existing ApplicationIsValid() gate still owns
     * this, boot_decision.h only ever narrows it further. */
    TEST_ASSERT(BootDecision_ShouldJump(0, 0) == 0, "invalid app never jumps, attempts=0");
    TEST_ASSERT(BootDecision_ShouldJump(0, 999) == 0, "invalid app never jumps, attempts=999");

    /* A valid application jumps while under the real threshold - every
     * value from 0 up to (but not including) BOOT_MAX_ATTEMPTS. */
    for (uint32_t attempts = 0; attempts < BOOT_MAX_ATTEMPTS; attempts++) {
        TEST_ASSERT(BootDecision_ShouldJump(1, attempts) == 1, "valid app jumps under the real threshold");
    }

    /* Exactly AT the threshold - and beyond - refuses. */
    TEST_ASSERT(BootDecision_ShouldJump(1, BOOT_MAX_ATTEMPTS) == 0, "valid app refused exactly at the real threshold");
    TEST_ASSERT(BootDecision_ShouldJump(1, BOOT_MAX_ATTEMPTS + 1) == 0, "valid app refused beyond the real threshold");
    TEST_ASSERT(BootDecision_ShouldJump(1, 0xFFFFFFFFu) == 0, "an implausibly large real count still refuses, never wraps into jumping");

    /* A real, honest sanity check on the threshold itself - never 0 (an
     * app would never even get its first real chance) and never absurdly
     * large (defeats the whole point). */
    TEST_ASSERT(BOOT_MAX_ATTEMPTS >= 1, "BOOT_MAX_ATTEMPTS must allow at least one real attempt");
    TEST_ASSERT(BOOT_MAX_ATTEMPTS <= 10, "BOOT_MAX_ATTEMPTS must stay a real, small, deliberate number");

    /* Only a WATCHDOG reset counts against the application: a board switched off and on, reset with its button or restarted by software is a clean start, so a healthy
     * board that nobody confirms never reaches the limit (it used to be parked at its fourth start). */
    TEST_ASSERT(BootDecision_EffectiveAttempts(2, BOOT_RESET_CLEAN) == 0, "a clean start ignores the stored count");
    TEST_ASSERT(BootDecision_EffectiveAttempts(2, BOOT_RESET_WATCHDOG) == 2, "a watchdog reset keeps the stored count");
    TEST_ASSERT(BootDecision_NextAttempts(0, BOOT_RESET_CLEAN) == 0, "a clean start leaves the count at 0 (nothing to write)");
    TEST_ASSERT(BootDecision_NextAttempts(1, BOOT_RESET_WATCHDOG) == 2, "a watchdog reset adds one");
    TEST_ASSERT(BootDecision_NextAttempts(0xFFFFFFFFu, BOOT_RESET_WATCHDOG) == 0xFFFFFFFFu, "the count never wraps");

    /* Ten power-ups of a healthy board: always jumps, the stored count never leaves 0. */
    {
        uint32_t stored = 0;
        for (int boot = 0; boot < 10; boot++) {
            uint32_t effective = BootDecision_EffectiveAttempts(stored, BOOT_RESET_CLEAN);
            TEST_ASSERT(BootDecision_ShouldJump(1, effective) == 1, "a healthy board is jumped to at every power-up");
            stored = BootDecision_NextAttempts(effective, BOOT_RESET_CLEAN);
        }
        TEST_ASSERT(stored == 0, "ten clean power-ups leave the count at 0");
    }

    /* A crashing application: the first start is clean, then the watchdog resets it again and again. It is tried BOOT_MAX_ATTEMPTS times and then parked; a power-up tries it again. */
    {
        uint32_t stored = 0;
        uint32_t effective = BootDecision_EffectiveAttempts(stored, BOOT_RESET_CLEAN);
        TEST_ASSERT(BootDecision_ShouldJump(1, effective) == 1, "the clean start jumps");
        stored = BootDecision_NextAttempts(effective, BOOT_RESET_CLEAN);
        int jumps = 0;
        for (int boot = 0; boot < 10; boot++) {
            effective = BootDecision_EffectiveAttempts(stored, BOOT_RESET_WATCHDOG);
            if (!BootDecision_ShouldJump(1, effective)) break;
            jumps++;
            stored = BootDecision_NextAttempts(effective, BOOT_RESET_WATCHDOG);
        }
        TEST_ASSERT(jumps == BOOT_MAX_ATTEMPTS, "a crash-looping application is tried the limit number of times, then parked");
        effective = BootDecision_EffectiveAttempts(stored, BOOT_RESET_CLEAN);
        TEST_ASSERT(BootDecision_ShouldJump(1, effective) == 1, "a power-up gives a parked board another try");
    }

    printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
