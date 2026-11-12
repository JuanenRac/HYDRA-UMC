/* =============================================================================
 * HYDRA-UMC Firmware - host test of clock_config.h (the clock set-up of the CM7 of the H757 board)
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * The RCC is a double whose ready flags follow the enable bits (or do not, for the failure cases). Every write and every set-bits is compared with the golden plan of the
 * Python model (tests/rcc_plan_golden.h), the fields of the registers are decoded here, and the failures must end the set-up before the core clock is switched.
 * Build with -DHYDRA_PLATFORM_H757. Nothing here touches a clock controller.
 */
#include <stdio.h>
#include <string.h>
#include "clock_config.h"
#include "rcc_plan_golden.h"

#if !defined(HYDRA_PLATFORM_H757)
#error "this test is for the H757 platform: build it with -DHYDRA_PLATFORM_H757"
#endif

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } } while (0)

typedef struct {
    uint32_t reg[10];
    struct { int kind; int reg; unsigned long value; } log[32];
    unsigned count;
    unsigned long reads;
    int hse_never_ready, pll_never_ready, switch_never_done;
    unsigned cfgr_writes;
    unsigned long latency_hz;
} Rcc;

static void note(Rcc *r, int kind, int reg, unsigned long value)
{
    if (r->count < 32u) { r->log[r->count].kind = kind; r->log[r->count].reg = reg; r->log[r->count].value = value; r->count++; }
}
static void op_latency(void *ctx, uint32_t hclk_hz) { Rcc *r = (Rcc *)ctx; r->latency_hz = hclk_hz; note(r, 0, 0, hclk_hz); }
static void op_write(void *ctx, RccReg reg, uint32_t value)
{
    Rcc *r = (Rcc *)ctx;
    r->reg[reg] = value;
    if (reg == RCC_REG_CFGR) r->cfgr_writes++;
    note(r, 1, (int)reg, value);
}
static void op_set(void *ctx, RccReg reg, uint32_t mask) { Rcc *r = (Rcc *)ctx; r->reg[reg] |= mask; note(r, 2, (int)reg, mask); }
static uint32_t op_read(void *ctx, RccReg reg)
{
    Rcc *r = (Rcc *)ctx;
    r->reads++;
    uint32_t v = r->reg[reg];
    if (reg == RCC_REG_CR) {
        if ((v & RCC_CR_HSEON) && !r->hse_never_ready) v |= RCC_CR_HSERDY;
        if ((v & RCC_CR_PLL1ON) && !r->pll_never_ready) v |= RCC_CR_PLL1RDY;
        if ((v & RCC_CR_PLL2ON) && !r->pll_never_ready) v |= RCC_CR_PLL2RDY;
    } else if (reg == RCC_REG_CFGR && !r->switch_never_done) {
        v = (v & ~(7UL << 3)) | ((v & 7UL) << 3);   /* the status field follows the switch */
    }
    return v;
}

static const RccOps OPS = {op_latency, op_write, op_set, op_read};

int main(void)
{
    static Rcc rcc;

    /* success: the writes are the golden plan, step by step */
    memset(&rcc, 0, sizeof rcc);
    CHECK(Rcc_Configure(&OPS, &rcc) == RCC_OK, "the set-up succeeds");
    CHECK(rcc.count == sizeof RCC_GOLDEN / sizeof RCC_GOLDEN[0], "as many steps as the plan");
    for (unsigned i = 0; i < rcc.count && i < sizeof RCC_GOLDEN / sizeof RCC_GOLDEN[0]; i++) {
        CHECK(rcc.log[i].kind == RCC_GOLDEN[i].kind && rcc.log[i].reg == RCC_GOLDEN[i].reg && rcc.log[i].value == RCC_GOLDEN[i].value, "the step is the golden one");
    }
    CHECK(rcc.latency_hz == HYDRA_CLOCK_HCLK_HZ, "the flash latency is asked for the bus clock");
    CHECK(rcc.log[0].kind == 0, "the flash latency comes first, before the clock is raised");
    CHECK(rcc.log[rcc.count - 1].kind == 1 && rcc.log[rcc.count - 1].reg == RCC_REG_CFGR, "the switch of the core clock is the last write");
    CHECK(rcc.cfgr_writes == 1u, "the core clock is switched once");

    /* the fields of the registers, decoded */
    const uint32_t sel = rcc.reg[RCC_REG_PLLCKSELR];
    CHECK((sel & 3UL) == 2UL && ((sel >> 4) & 0x3FUL) == HYDRA_CLOCK_PLL1_DIVM && ((sel >> 12) & 0x3FUL) == HYDRA_CLOCK_PLL2_DIVM, "PLLCKSELR: crystal source and the input dividers");
    CHECK(((sel >> 20) & 0x3FUL) == 32UL, "PLLCKSELR: PLL3 keeps its reset divider (off)");
    CHECK((rcc.reg[RCC_REG_PLL1DIVR] & 0x1FFUL) + 1UL == HYDRA_CLOCK_PLL1_DIVN, "PLL1DIVR: N");
    CHECK(((rcc.reg[RCC_REG_PLL1DIVR] >> 9) & 0x7FUL) + 1UL == HYDRA_CLOCK_SYSCLK_DIV, "PLL1DIVR: P");
    CHECK((rcc.reg[RCC_REG_PLL2DIVR] & 0x1FFUL) + 1UL == HYDRA_CLOCK_PLL2_DIVN, "PLL2DIVR: N");
    CHECK(((rcc.reg[RCC_REG_PLL2DIVR] >> 16) & 0x7FUL) + 1UL == HYDRA_CLOCK_PLL2_Q_DIV, "PLL2DIVR: Q, the FDCAN kernel clock");
    CHECK(((rcc.reg[RCC_REG_PLLCFGR] >> 16) & 7UL) == 1UL && ((rcc.reg[RCC_REG_PLLCFGR] >> 19) & 7UL) == 7UL, "PLLCFGR: PLL1 P and PLL2 P, Q and R are on, nothing else");
    CHECK(((rcc.reg[RCC_REG_PLLCFGR] >> 22) & 7UL) == 0UL, "PLLCFGR: the outputs of PLL3 are off");
    CHECK(((rcc.reg[RCC_REG_D2CCIP1R] >> 28) & 3UL) == 2UL, "D2CCIP1R: the FDCAN kernel clock is PLL2_Q");
    CHECK((rcc.reg[RCC_REG_CR] & (RCC_CR_PLL1ON | RCC_CR_PLL2ON | RCC_CR_HSEON)) == (RCC_CR_PLL1ON | RCC_CR_PLL2ON | RCC_CR_HSEON), "CR: crystal and both PLLs on");
    CHECK((rcc.reg[RCC_REG_CR] & (1UL << 28)) == 0UL, "CR: PLL3 is not started");

    /* the crystal never gets ready: error, no PLL touched, the core clock not switched */
    memset(&rcc, 0, sizeof rcc);
    rcc.hse_never_ready = 1;
    CHECK(Rcc_Configure(&OPS, &rcc) == RCC_ERR_HSE, "crystal never ready: error");
    CHECK(rcc.cfgr_writes == 0u && rcc.reg[RCC_REG_PLLCFGR] == 0u && (rcc.reg[RCC_REG_CR] & (RCC_CR_PLL1ON | RCC_CR_PLL2ON)) == 0UL, "no PLL is set up and the core clock is not switched");
    CHECK(rcc.reads <= HYDRA_CLOCK_POLL_LIMIT, "the wait is bounded by the limit of polls");

    /* a PLL never gets ready: error, the core clock is not switched */
    memset(&rcc, 0, sizeof rcc);
    rcc.pll_never_ready = 1;
    CHECK(Rcc_Configure(&OPS, &rcc) == RCC_ERR_PLL, "PLL never ready: error");
    CHECK(rcc.cfgr_writes == 0u && rcc.reg[RCC_REG_D1CFGR] == 0u, "the bus dividers and the core clock are left alone");

    /* the switch is not confirmed: error after the write */
    memset(&rcc, 0, sizeof rcc);
    rcc.switch_never_done = 1;
    CHECK(Rcc_Configure(&OPS, &rcc) == RCC_ERR_SWITCH && rcc.cfgr_writes == 1u, "switch not confirmed: error");

    if (failures == 0) printf("CLOCK_CONFIG=PASS\n");
    return failures == 0 ? 0 : 1;
}
