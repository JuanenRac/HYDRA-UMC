/* =============================================================================
 * HYDRA-UMC Firmware - Minimal host-side test harness: test_runner.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0 - see LICENSE
 * =============================================================================
 * No external test framework, on purpose: these tests compile and run with the
 * host's plain gcc/cc, never arm-none-eabi-gcc. They exercise the HAL-free
 * logic pulled out of the MCU sources (relay_rx_queue.h) so a real STM32 board
 * is not needed to run them. Final on-target verification (E-STOP timing,
 * watchdog, physical CAN) still needs the boards - see the repo README.
 */
#ifndef TEST_RUNNER_H
#define TEST_RUNNER_H

#include <stdio.h>

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("  FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
            (*failures)++; \
        } \
    } while (0)

void run_relay_rx_queue_tests(int *failures);

#endif /* TEST_RUNNER_H */
