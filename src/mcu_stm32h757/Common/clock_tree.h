// =============================================================================
// HYDRA-UMC Kinematic Brain - clock tree of the STM32H757 board, checked at compile time
// Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
// GPL-3.0-or-later - see repo root LICENSE
//
// Header-only and free of the HAL: it takes the numbers of the platform profile (25 MHz crystal, the three PLLs and their output dividers, the kernel clocks of
// the peripherals) and refuses to compile when they do not add up: a PLL output that is not what the table says, a stack-bus bit rate that is not 1 Mbit/s at
// the FDCAN kernel clock, a kernel clock that is not the output of the PLL it is taken from. The code that writes the RCC registers (a part of the
// SystemClock_Config of the CM7) includes this header and uses the same macros, so the registers and the checks cannot drift apart.
// Only for the H757 platform: on the H745 platform the header defines nothing. Nothing here has run on a board.
// =============================================================================
#ifndef HYDRA_CLOCK_TREE_H
#define HYDRA_CLOCK_TREE_H

#include "platform_profile.h"

#if defined(HYDRA_PLATFORM_H757)

// The frequency of one PLL output: hse / divm * divn / divider (the VCO is hse / divm * divn).
#define HYDRA_PLL_OUT_HZ(divm, divn, divider) ((HYDRA_CLOCK_HSE_HZ / (divm)) * (divn) / (divider))

// PLL1 and PLL2 (the ones the bus and the cores depend on): the reference clock (hse / divm) is inside 1..16 MHz and the VCO inside 192..836 MHz (wide range). PLL3 (display clock) is only checked for its outputs: its profile values give a 25 MHz reference, which the display phase has to confirm against the board file
#define HYDRA_PLL_REF_HZ(divm) (HYDRA_CLOCK_HSE_HZ / (divm))
#define HYDRA_PLL_VCO_HZ(divm, divn) (HYDRA_PLL_REF_HZ(divm) * (divn))

#define HYDRA_PLL1_VCO_HZ HYDRA_PLL_VCO_HZ(HYDRA_CLOCK_PLL1_DIVM, HYDRA_CLOCK_PLL1_DIVN)
#define HYDRA_PLL2_VCO_HZ HYDRA_PLL_VCO_HZ(HYDRA_CLOCK_PLL2_DIVM, HYDRA_CLOCK_PLL2_DIVN)
#define HYDRA_PLL3_VCO_HZ HYDRA_PLL_VCO_HZ(HYDRA_CLOCK_PLL3_DIVM, HYDRA_CLOCK_PLL3_DIVN)

_Static_assert(HYDRA_CLOCK_HSE_HZ == 25000000UL, "the crystal of the board is 25 MHz");
_Static_assert(HYDRA_PLL_REF_HZ(HYDRA_CLOCK_PLL1_DIVM) >= 1000000UL && HYDRA_PLL_REF_HZ(HYDRA_CLOCK_PLL1_DIVM) <= 16000000UL, "PLL1 reference out of range");
_Static_assert(HYDRA_PLL_REF_HZ(HYDRA_CLOCK_PLL2_DIVM) >= 1000000UL && HYDRA_PLL_REF_HZ(HYDRA_CLOCK_PLL2_DIVM) <= 16000000UL, "PLL2 reference out of range");
_Static_assert(HYDRA_PLL1_VCO_HZ >= 192000000UL && HYDRA_PLL1_VCO_HZ <= 836000000UL, "PLL1 VCO out of range");
_Static_assert(HYDRA_PLL2_VCO_HZ >= 192000000UL && HYDRA_PLL2_VCO_HZ <= 836000000UL, "PLL2 VCO out of range");

_Static_assert(HYDRA_PLL_OUT_HZ(HYDRA_CLOCK_PLL1_DIVM, HYDRA_CLOCK_PLL1_DIVN, HYDRA_CLOCK_SYSCLK_DIV) == HYDRA_CLOCK_SYSCLK_HZ, "SYSCLK is not the output of PLL1");
_Static_assert(HYDRA_CLOCK_SYSCLK_HZ / 2UL == HYDRA_CLOCK_HCLK_HZ, "HCLK is half of SYSCLK");
_Static_assert(HYDRA_CLOCK_CPU2_HZ == HYDRA_CLOCK_HCLK_HZ, "the second core runs at HCLK");
_Static_assert(HYDRA_PLL_OUT_HZ(HYDRA_CLOCK_PLL2_DIVM, HYDRA_CLOCK_PLL2_DIVN, HYDRA_CLOCK_PLL2_P_DIV) == HYDRA_CLOCK_PLL2_P_HZ, "PLL2_P");
_Static_assert(HYDRA_PLL_OUT_HZ(HYDRA_CLOCK_PLL2_DIVM, HYDRA_CLOCK_PLL2_DIVN, HYDRA_CLOCK_PLL2_Q_DIV) == HYDRA_CLOCK_PLL2_Q_HZ, "PLL2_Q");
_Static_assert(HYDRA_PLL_OUT_HZ(HYDRA_CLOCK_PLL2_DIVM, HYDRA_CLOCK_PLL2_DIVN, HYDRA_CLOCK_PLL2_R_DIV) == HYDRA_CLOCK_PLL2_R_HZ, "PLL2_R");
_Static_assert(HYDRA_PLL_OUT_HZ(HYDRA_CLOCK_PLL3_DIVM, HYDRA_CLOCK_PLL3_DIVN, HYDRA_CLOCK_PLL3_P_DIV) == HYDRA_CLOCK_PLL3_P_HZ, "PLL3_P");
_Static_assert(HYDRA_PLL_OUT_HZ(HYDRA_CLOCK_PLL3_DIVM, HYDRA_CLOCK_PLL3_DIVN, HYDRA_CLOCK_PLL3_Q_DIV) == HYDRA_CLOCK_PLL3_Q_HZ, "PLL3_Q");
_Static_assert(HYDRA_PLL_OUT_HZ(HYDRA_CLOCK_PLL3_DIVM, HYDRA_CLOCK_PLL3_DIVN, HYDRA_CLOCK_PLL3_R_DIV) == HYDRA_CLOCK_PLL3_R_HZ, "PLL3_R");

// the kernel clock of the FDCAN is PLL2_Q and gives the 1 Mbit/s of the stack bus with the prescaler and the segments of the profile
_Static_assert(HYDRA_CLOCK_KERNEL_FDCAN_HZ == HYDRA_CLOCK_PLL2_Q_HZ, "the FDCAN kernel clock is PLL2_Q");
_Static_assert(HYDRA_STACK_BUS_KERNEL_CLOCK_HZ == HYDRA_CLOCK_KERNEL_FDCAN_HZ, "the bit timing assumes the kernel clock the clock tree gives");
_Static_assert(HYDRA_CLOCK_KERNEL_FDCAN_HZ / (HYDRA_STACK_BUS_PRESCALER * (1U + HYDRA_STACK_BUS_TIME_SEG1 + HYDRA_STACK_BUS_TIME_SEG2)) == HYDRA_STACK_BUS_BIT_RATE_BPS,
               "the stack bus is not at its bit rate with this clock");
_Static_assert(HYDRA_CLOCK_KERNEL_FDCAN_HZ % (HYDRA_STACK_BUS_PRESCALER * (1U + HYDRA_STACK_BUS_TIME_SEG1 + HYDRA_STACK_BUS_TIME_SEG2)) == 0U, "the bit rate is not exact");
// the I2C4, SPI4, SPI5 and SPI6 kernel clocks follow the APB / PLL2 values of the profile
_Static_assert(HYDRA_CLOCK_KERNEL_I2C4_HZ == HYDRA_CLOCK_APB4_HZ, "I2C4 runs from the APB4 clock");
_Static_assert(HYDRA_CLOCK_KERNEL_SPI5_HZ == HYDRA_CLOCK_APB2_HZ, "SPI5 runs from the APB2 clock");
_Static_assert(HYDRA_CLOCK_KERNEL_SPI6_HZ == HYDRA_CLOCK_APB4_HZ, "SPI6 runs from the APB4 clock");

#endif // HYDRA_PLATFORM_H757

#endif // HYDRA_CLOCK_TREE_H
