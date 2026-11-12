/* =============================================================================
 * HYDRA-UMC Firmware - host test of sdram_init.h (the start-up of the external SDRAM of the H757 board)
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * The FMC is a double that logs every register write and every wait. The sequence is compared with the golden plan of the Python model (tests/sdram_plan_golden.h),
 * and the bits of the registers are decoded here with the layout of the FMC. Build with -DHYDRA_PLATFORM_H757; the variants -DHYDRA_SDRAM_BANK2_ENABLED=0 and
 * -DHYDRA_SDRAM_BANK1_ENABLED=0 check the banks that are switched off. Nothing here touches a memory controller.
 */
#include <stdio.h>
#include "sdram_init.h"
#include "sdram_plan_golden.h"

#if !defined(HYDRA_PLATFORM_H757)
#error "this test is for the H757 platform: build it with -DHYDRA_PLATFORM_H757"
#endif

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } } while (0)

typedef struct {
    struct { int is_wait; int reg; unsigned long value; } log[32];
    unsigned count;
    unsigned busy_checks;
    unsigned fail_busy_at;   /* the busy wait that times out (0: never) */
} Fmc;

static void op_write(void *ctx, SdramReg reg, uint32_t value)
{
    Fmc *f = (Fmc *)ctx;
    if (f->count < 32u) { f->log[f->count].is_wait = 0; f->log[f->count].reg = (int)reg; f->log[f->count].value = value; f->count++; }
}
static int op_busy(void *ctx)
{
    Fmc *f = (Fmc *)ctx;
    f->busy_checks++;
    return !(f->fail_busy_at != 0u && f->busy_checks == f->fail_busy_at);
}
static void op_wait(void *ctx, uint32_t us)
{
    Fmc *f = (Fmc *)ctx;
    if (f->count < 32u) { f->log[f->count].is_wait = 1; f->log[f->count].reg = 0; f->log[f->count].value = us; f->count++; }
}

static const SdramOps OPS = {op_write, op_busy, op_wait};

int main(void)
{
    Fmc fmc = {{{0, 0, 0}}, 0, 0, 0};
    CHECK(Sdram_Start(&OPS, &fmc) == SDRAM_OK, "the start-up succeeds");

#if HYDRA_SDRAM_BANK1_ENABLED && HYDRA_SDRAM_BANK2_ENABLED
    /* both banks on: the log is the golden plan of the model, step by step */
    CHECK(fmc.count == sizeof SDRAM_GOLDEN / sizeof SDRAM_GOLDEN[0], "as many steps as the plan");
    for (unsigned i = 0; i < fmc.count && i < sizeof SDRAM_GOLDEN / sizeof SDRAM_GOLDEN[0]; i++) {
        CHECK(fmc.log[i].is_wait == SDRAM_GOLDEN[i].is_wait && fmc.log[i].reg == SDRAM_GOLDEN[i].reg && fmc.log[i].value == SDRAM_GOLDEN[i].value, "the step is the golden one");
    }
    CHECK(SDRAM_SDCMR(SDRAM_MODE_CLOCK_ENABLE, 1U, 0U) == 25UL, "clock enable goes to both banks");
#endif

    /* the order of the JEDEC sequence, whatever the banks */
    CHECK(fmc.log[4].reg == SDRAM_REG_SDCMR && (fmc.log[4].value & 7UL) == SDRAM_MODE_CLOCK_ENABLE, "step 5: clock enable");
    CHECK(fmc.log[5].is_wait && fmc.log[5].value >= 100UL, "step 6: at least 100 us");
    CHECK((fmc.log[6].value & 7UL) == SDRAM_MODE_PALL, "step 7: precharge all");
    CHECK((fmc.log[7].value & 7UL) == SDRAM_MODE_AUTO_REFRESH && ((fmc.log[7].value >> 5) & 0xFUL) == HYDRA_SDRAM_AUTO_REFRESH_CYCLES - 1UL, "step 8: auto-refresh cycles");
    CHECK((fmc.log[8].value & 7UL) == SDRAM_MODE_LOAD_MODE && (fmc.log[8].value >> 9) == SDRAM_MODE_REGISTER_VALUE, "step 9: load mode register");
    CHECK(fmc.log[9].reg == SDRAM_REG_SDRTR && (fmc.log[9].value >> 1) == SDRAM_REFRESH_COUNT, "step 10: the refresh rate is last");
    CHECK(fmc.busy_checks == 4u, "the controller is checked for BUSY before each of the four commands");

    /* the banks the commands go to follow the switches */
    const unsigned long targets = (fmc.log[4].value >> 3) & 3UL;
    CHECK(targets == ((HYDRA_SDRAM_BANK2_ENABLED ? 1UL : 0UL) | (HYDRA_SDRAM_BANK1_ENABLED ? 2UL : 0UL)), "the target bits are the enabled banks");
    for (unsigned i = 4; i < 9; i++) {
        if (!fmc.log[i].is_wait) CHECK(((fmc.log[i].value >> 3) & 3UL) == targets, "every command goes to the same banks");
    }

    /* the bits of the registers, decoded with the layout of the FMC */
    const unsigned long cr = fmc.log[0].value;
    CHECK((cr & 3UL) == HYDRA_SDRAM_COLUMN_BITS - 8UL, "SDCR: columns");
    CHECK(((cr >> 2) & 3UL) == HYDRA_SDRAM_ROW_BITS - 11UL, "SDCR: rows");
    CHECK(((cr >> 4) & 3UL) == 2UL, "SDCR: 32-bit bus");
    CHECK(((cr >> 6) & 1UL) == 1UL, "SDCR: four internal banks");
    CHECK(((cr >> 7) & 3UL) == HYDRA_SDRAM_CAS_LATENCY, "SDCR: CAS");
    CHECK(((cr >> 10) & 3UL) == HYDRA_SDRAM_SDCLK_PERIOD_HCLK, "SDCR: SDRAM clock period");
    CHECK(fmc.log[2].value == (cr & 0x3FFUL), "SDCR2: only the bits bank 2 keeps for itself");
    const unsigned long tr = fmc.log[1].value;
    CHECK((tr & 0xFUL) == HYDRA_SDRAM_TMRD - 1UL && ((tr >> 12) & 0xFUL) == HYDRA_SDRAM_TRC - 1UL && ((tr >> 20) & 0xFUL) == HYDRA_SDRAM_TRP - 1UL, "SDTR: timings are cycles less one");
    CHECK(((fmc.log[3].value >> 12) & 0xFUL) == 0UL && ((fmc.log[3].value >> 20) & 0xFUL) == 0UL, "SDTR2: tRC and tRP come from bank 1");

    /* the refresh covers every row within the refresh period (a row refresh each count + margin SDRAM clock cycles) */
    CHECK((SDRAM_REFRESH_COUNT + HYDRA_SDRAM_REFRESH_MARGIN_CYCLES) * (1ULL << HYDRA_SDRAM_ROW_BITS) * 1000ULL <= (unsigned long long)HYDRA_SDRAM_REFRESH_PERIOD_MS * SDRAM_SDCLK_HZ,
          "all rows are refreshed in the refresh period");

    /* the geometry holds the windows of the profile */
    CHECK(HYDRA_SDRAM_CAPACITY_BYTES == 134217728UL, "the geometry holds 128 MiB per bank");

    /* a BUSY that never falls ends the start-up at that command, and nothing is written after it */
    for (unsigned at = 1; at <= 4u; at++) {
        Fmc broken = {{{0, 0, 0}}, 0, 0, at};
        CHECK(Sdram_Start(&OPS, &broken) == SDRAM_ERR_BUSY_TIMEOUT, "busy time-out ends the start-up");
        CHECK(broken.busy_checks == at, "no command is tried after the time-out");
        CHECK(broken.count < fmc.count, "less than the whole sequence was written");
        for (unsigned i = 0; i < broken.count; i++) CHECK(!(broken.log[i].reg == SDRAM_REG_SDRTR && !broken.log[i].is_wait), "the refresh rate is not written when the start-up failed");
    }

    if (failures == 0) printf("SDRAM_INIT=PASS\n");
    return failures == 0 ? 0 : 1;
}
