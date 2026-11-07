/* =============================================================================
 * HYDRA-UMC - src/mcu_stm32g474/fdcan_clock.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * The kernel clock of the FDCAN peripheral of the STM32G474, made explicit (HAL-free, host-testable).
 *
 * The bit timing of every FDCAN set-up of the firmware (prescaler 1, 13 + 2 time segments, 16 quanta per bit) gives 1 Mbit/s only if the kernel
 * clock of the peripheral is 16 MHz. The clock tree is still the HSI reset default (16 MHz, no PLL, no HSE). The selector of the FDCAN kernel
 * clock (RCC_CCIPR, FDCANSEL, RM0440) resets to 00 = HSE, and no HSE is running: left alone, the peripheral may be fed by a clock that does not
 * exist. Choosing PCLK1 (which is the HSI at divider 1) states the assumption of the bit timing in the code instead of in a comment.
 *
 * The firmware does this only when the build sets HYDRA_FDCAN_KERNEL_CLOCK_EXPLICIT to 1 (default 0: the binary does not change). Turn it on
 * after row 10 of docs/BENCH_TESTS.md measured the clock of a cold power-up on a board.
 * ========================================================================== */
#ifndef HYDRA_UMC_FDCAN_CLOCK_H
#define HYDRA_UMC_FDCAN_CLOCK_H

#include <stdint.h>

#ifndef HYDRA_FDCAN_KERNEL_CLOCK_EXPLICIT
#define HYDRA_FDCAN_KERNEL_CLOCK_EXPLICIT 0
#endif

/* The three sources of RCC_CCIPR.FDCANSEL. */
#define FDCAN_CLOCK_SRC_HSE   0u
#define FDCAN_CLOCK_SRC_PLLQ  1u
#define FDCAN_CLOCK_SRC_PCLK1 2u

/* What the firmware asks for, and the clock its bit timing assumes. */
#define FDCAN_CLOCK_WANTED_SOURCE FDCAN_CLOCK_SRC_PCLK1
#define FDCAN_CLOCK_ASSUMED_HZ    16000000u

/* The state of the clock tree, as far as the FDCAN cares. A clock that is not running is 0 Hz. */
typedef struct {
    uint32_t hse_hz;   /* 0 when the HSE oscillator is off (today: always) */
    uint32_t pllq_hz;  /* 0 when the PLL is off (today: always) */
    uint32_t pclk1_hz; /* the APB1 clock: HSI 16 MHz at divider 1 today */
} FdcanClockTree_t;

/* The frequency the peripheral is fed with for a selector value; 0 for a source that does not run or a value that selects nothing. */
static inline uint32_t FdcanClock_KernelHz(uint32_t source, const FdcanClockTree_t *tree)
{
    switch (source) {
    case FDCAN_CLOCK_SRC_HSE:   return tree->hse_hz;
    case FDCAN_CLOCK_SRC_PLLQ:  return tree->pllq_hz;
    case FDCAN_CLOCK_SRC_PCLK1: return tree->pclk1_hz;
    default:                    return 0u;
    }
}

/* Bit rate for a timing: kernel / (prescaler * (1 + seg1 + seg2)); 0 when the clock does not run or the timing is empty. */
static inline uint32_t FdcanClock_BitRate(uint32_t kernel_hz, uint32_t prescaler, uint32_t seg1, uint32_t seg2)
{
    const uint32_t quanta = prescaler * (1u + seg1 + seg2);
    return quanta == 0u ? 0u : kernel_hz / quanta;
}

/* 1 when the selected source gives the bit rate the stack bus is meant to run at, for the timing the firmware sets. */
static inline uint8_t FdcanClock_RateIsRight(uint32_t source, const FdcanClockTree_t *tree, uint32_t prescaler, uint32_t seg1, uint32_t seg2,
                                             uint32_t wanted_bps)
{
    return (uint8_t)(wanted_bps != 0u && FdcanClock_BitRate(FdcanClock_KernelHz(source, tree), prescaler, seg1, seg2) == wanted_bps);
}

/* The clock tree the firmware has today (HSI 16 MHz as system clock, APB1 at divider 1, no HSE, no PLL). */
static inline FdcanClockTree_t FdcanClock_TreeAfterReset(void)
{
    FdcanClockTree_t tree = {0u, 0u, 16000000u};
    return tree;
}

#endif /* HYDRA_UMC_FDCAN_CLOCK_H */
