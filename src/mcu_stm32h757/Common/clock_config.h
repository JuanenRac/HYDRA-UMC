// =============================================================================
// HYDRA-UMC Kinematic Brain - clock set-up of the CM7 of the STM32H757 board, header-only and free of the HAL
// Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
// GPL-3.0-or-later - see repo root LICENSE
//
// The C twin of tools/rcc_plan.py: from the macros of the platform profile (25 MHz crystal, PLL1 and PLL2 and their dividers, the AHB and APB dividers that give the clocks of
// the table) it computes the values of the RCC registers and runs the sequence: the flash latency for the bus clock, the crystal on and ready, the PLL inputs and outputs, both
// PLLs on and ready, the bus dividers, the FDCAN kernel clock from PLL2_Q and the switch of the core clock to PLL1. The registers are reached only through the callbacks of
// RccOps, so the host test runs the sequence against a double and compares every write with the golden plan of the Python model.
//
// A ready flag that does not rise within the limit of polls ends the set-up with an error and the core clock is NOT switched: the core stays on the clock it had. The display
// clock (PLL3) is not set up here (its reference in the profile is outside the range of the PLL and has to be confirmed first). The layout of the registers is that of the
// STM32H7 family as the HAL of the family uses it and is still to be confirmed with the reference manual. Only for the H757 platform. Nothing here has run on a board.
// =============================================================================
#ifndef HYDRA_CLOCK_CONFIG_H
#define HYDRA_CLOCK_CONFIG_H

#include <stdint.h>
#include "clock_tree.h"

#if defined(HYDRA_PLATFORM_H757)

#define RCC_CR_HSEON   (1UL << 16)
#define RCC_CR_HSERDY  (1UL << 17)
#define RCC_CR_PLL1ON  (1UL << 24)
#define RCC_CR_PLL1RDY (1UL << 25)
#define RCC_CR_PLL2ON  (1UL << 26)
#define RCC_CR_PLL2RDY (1UL << 27)
#define RCC_CFGR_SW_PLL1 3UL
#define RCC_FDCANSEL_PLL2_Q 2UL
#define RCC_PLLSRC_HSE 2UL
#define RCC_PLL3_RESET_DIVM 32UL   /* the reset value of DIVM3: PLL3 stays off */

/* the divider of an AHB or APB clock as the register holds it (the divider 1 is 0) */
#define RCC_AHB_DIV(d) ((d) == 1UL ? 0UL : (d) == 2UL ? 8UL : (d) == 4UL ? 9UL : (d) == 8UL ? 10UL : (d) == 16UL ? 11UL : 99UL)
#define RCC_APB_DIV(d) ((d) == 1UL ? 0UL : (d) == 2UL ? 4UL : (d) == 4UL ? 5UL : (d) == 8UL ? 6UL : (d) == 16UL ? 7UL : 99UL)
#define RCC_HPRE_DIV   (HYDRA_CLOCK_SYSCLK_HZ / HYDRA_CLOCK_HCLK_HZ)
#define RCC_D1PPRE_DIV (HYDRA_CLOCK_HCLK_HZ / HYDRA_CLOCK_APB3_HZ)
#define RCC_D2PPRE1_DIV (HYDRA_CLOCK_HCLK_HZ / HYDRA_CLOCK_APB1_HZ)
#define RCC_D2PPRE2_DIV (HYDRA_CLOCK_HCLK_HZ / HYDRA_CLOCK_APB2_HZ)
#define RCC_D3PPRE_DIV (HYDRA_CLOCK_HCLK_HZ / HYDRA_CLOCK_APB4_HZ)
_Static_assert(RCC_AHB_DIV(RCC_HPRE_DIV) != 99UL, "the AHB divider is not one the register has");
_Static_assert(RCC_APB_DIV(RCC_D1PPRE_DIV) != 99UL && RCC_APB_DIV(RCC_D2PPRE1_DIV) != 99UL && RCC_APB_DIV(RCC_D2PPRE2_DIV) != 99UL && RCC_APB_DIV(RCC_D3PPRE_DIV) != 99UL,
               "an APB divider is not one the register has");
_Static_assert(HYDRA_CLOCK_CPU2_HZ == HYDRA_CLOCK_HCLK_HZ, "the second core runs at the bus clock");

/* the input range (RGE) and the VCO range (VCOSEL) of a PLL from its reference */
#define RCC_RGE(ref) ((ref) <= 2000000UL ? 0UL : (ref) <= 4000000UL ? 1UL : (ref) <= 8000000UL ? 2UL : 3UL)
#define RCC_VCOSEL(vco) ((vco) >= 192000000UL ? 0UL : 1UL)
#define RCC_PLL1_REF (HYDRA_CLOCK_HSE_HZ / HYDRA_CLOCK_PLL1_DIVM)
#define RCC_PLL2_REF (HYDRA_CLOCK_HSE_HZ / HYDRA_CLOCK_PLL2_DIVM)

#define RCC_PLLCKSELR_VALUE (RCC_PLLSRC_HSE | ((unsigned long)HYDRA_CLOCK_PLL1_DIVM << 4) | ((unsigned long)HYDRA_CLOCK_PLL2_DIVM << 12) | (RCC_PLL3_RESET_DIVM << 20))
#define RCC_PLLCFGR_VALUE ((RCC_VCOSEL(HYDRA_PLL1_VCO_HZ) << 1) | (RCC_RGE(RCC_PLL1_REF) << 2) | (1UL << 16) | \
                           (RCC_VCOSEL(HYDRA_PLL2_VCO_HZ) << 5) | (RCC_RGE(RCC_PLL2_REF) << 6) | (1UL << 19) | (1UL << 20) | (1UL << 21))
#define RCC_PLL1DIVR_VALUE ((unsigned long)(HYDRA_CLOCK_PLL1_DIVN - 1U) | ((unsigned long)(HYDRA_CLOCK_SYSCLK_DIV - 1U) << 9))
#define RCC_PLL2DIVR_VALUE ((unsigned long)(HYDRA_CLOCK_PLL2_DIVN - 1U) | ((unsigned long)(HYDRA_CLOCK_PLL2_P_DIV - 1U) << 9) | \
                            ((unsigned long)(HYDRA_CLOCK_PLL2_Q_DIV - 1U) << 16) | ((unsigned long)(HYDRA_CLOCK_PLL2_R_DIV - 1U) << 24))
#define RCC_D1CFGR_VALUE (RCC_AHB_DIV(RCC_HPRE_DIV) | (RCC_APB_DIV(RCC_D1PPRE_DIV) << 4) | (RCC_AHB_DIV(1UL) << 8))
#define RCC_D2CFGR_VALUE ((RCC_APB_DIV(RCC_D2PPRE1_DIV) << 4) | (RCC_APB_DIV(RCC_D2PPRE2_DIV) << 8))
#define RCC_D3CFGR_VALUE (RCC_APB_DIV(RCC_D3PPRE_DIV) << 4)
#define RCC_D2CCIP1R_VALUE (RCC_FDCANSEL_PLL2_Q << 28)

typedef enum {
    RCC_REG_CR = 0, RCC_REG_PLLCKSELR, RCC_REG_PLLCFGR, RCC_REG_PLL1DIVR, RCC_REG_PLL2DIVR,
    RCC_REG_D1CFGR, RCC_REG_D2CFGR, RCC_REG_D3CFGR, RCC_REG_D2CCIP1R, RCC_REG_CFGR
} RccReg;

typedef enum { RCC_OK = 0, RCC_ERR_HSE = 1, RCC_ERR_PLL = 2, RCC_ERR_SWITCH = 3 } RccStatus;

typedef struct {
    void (*flash_latency)(void *ctx, uint32_t hclk_hz);       /* the wait states the bus clock needs, before the clock is raised */
    void (*write_reg)(void *ctx, RccReg reg, uint32_t value);
    void (*set_bits)(void *ctx, RccReg reg, uint32_t mask);
    uint32_t (*read_reg)(void *ctx, RccReg reg);
} RccOps;

static inline int Rcc_Wait(const RccOps *ops, void *ctx, RccReg reg, uint32_t mask, uint32_t want)
{
    for (unsigned long poll = 0; poll < HYDRA_CLOCK_POLL_LIMIT; poll++) {
        if ((ops->read_reg(ctx, reg) & mask) == want) return 1;
    }
    return 0;
}

static inline RccStatus Rcc_Configure(const RccOps *ops, void *ctx)
{
    ops->flash_latency(ctx, HYDRA_CLOCK_HCLK_HZ);
    ops->set_bits(ctx, RCC_REG_CR, RCC_CR_HSEON);
    if (!Rcc_Wait(ops, ctx, RCC_REG_CR, RCC_CR_HSERDY, RCC_CR_HSERDY)) return RCC_ERR_HSE;
    ops->write_reg(ctx, RCC_REG_PLLCKSELR, (uint32_t)RCC_PLLCKSELR_VALUE);
    ops->write_reg(ctx, RCC_REG_PLLCFGR, (uint32_t)RCC_PLLCFGR_VALUE);
    ops->write_reg(ctx, RCC_REG_PLL1DIVR, (uint32_t)RCC_PLL1DIVR_VALUE);
    ops->write_reg(ctx, RCC_REG_PLL2DIVR, (uint32_t)RCC_PLL2DIVR_VALUE);
    ops->set_bits(ctx, RCC_REG_CR, RCC_CR_PLL1ON | RCC_CR_PLL2ON);
    if (!Rcc_Wait(ops, ctx, RCC_REG_CR, RCC_CR_PLL1RDY | RCC_CR_PLL2RDY, RCC_CR_PLL1RDY | RCC_CR_PLL2RDY)) return RCC_ERR_PLL;
    ops->write_reg(ctx, RCC_REG_D1CFGR, (uint32_t)RCC_D1CFGR_VALUE);
    ops->write_reg(ctx, RCC_REG_D2CFGR, (uint32_t)RCC_D2CFGR_VALUE);
    ops->write_reg(ctx, RCC_REG_D3CFGR, (uint32_t)RCC_D3CFGR_VALUE);
    ops->write_reg(ctx, RCC_REG_D2CCIP1R, (uint32_t)RCC_D2CCIP1R_VALUE);
    ops->write_reg(ctx, RCC_REG_CFGR, (uint32_t)RCC_CFGR_SW_PLL1);
    /* the switch is confirmed by the status field (bits 5:3) showing PLL1 */
    if (!Rcc_Wait(ops, ctx, RCC_REG_CFGR, 7UL << 3, RCC_CFGR_SW_PLL1 << 3)) return RCC_ERR_SWITCH;
    return RCC_OK;
}

#endif // HYDRA_PLATFORM_H757

#endif // HYDRA_CLOCK_CONFIG_H
