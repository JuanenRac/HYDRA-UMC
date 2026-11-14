/* =============================================================================
 * HYDRA-UMC Firmware - host test of sdram_banks.h (which SDRAM banks may be used, with a probe of the optional second bank)
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Bank 2 is a RAM double with faults (a data line stuck, an address line stuck, a cell stuck, a window that is not a power of two). The decision must leave bank 2 on only
 * when it is enabled and, if the probe is on, when the memory tests pass; a bank that is not enabled is never touched. Build with -DHYDRA_PLATFORM_H757; the variants
 * -DHYDRA_SDRAM_BANK2_ENABLED=1 and -DHYDRA_SDRAM_BANK2_ENABLED=1 -DHYDRA_SDRAM_BANK2_PROBE=1 check the build wrappers. Nothing here touches a memory controller.
 */
#include <stdio.h>
#include "sdram_banks.h"

#if !defined(HYDRA_PLATFORM_H757)
#error "this test is for the H757 platform: build it with -DHYDRA_PLATFORM_H757"
#endif

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } } while (0)

#define WORDS SDRAM_BANKS_PROBE_WORDS

typedef struct {
    uint32_t cell[WORDS];
    uint32_t data_stuck_mask;      /* data lines that read as 1 */
    uint32_t addr_stuck0_mask;     /* address lines stuck at 0 */
    int stuck_cell;                /* a cell whose bit 4 reads as 0 (-1: none) */
    unsigned calls;
} Ram;

static uint32_t ram_read(void *ctx, uint32_t index)
{
    Ram *r = (Ram *)ctx;
    r->calls++;
    const uint32_t at = (index & ~r->addr_stuck0_mask) % WORDS;
    uint32_t value = r->cell[at] | r->data_stuck_mask;
    if ((int)at == r->stuck_cell) value &= ~0x10u;
    return value;
}

static void ram_write(void *ctx, uint32_t index, uint32_t value)
{
    Ram *r = (Ram *)ctx;
    r->calls++;
    r->cell[(index & ~r->addr_stuck0_mask) % WORDS] = value;
}

static const MemOps OPS = {ram_read, ram_write};

static void reset(Ram *r)
{
    for (uint32_t i = 0; i < WORDS; i++) r->cell[i] = 0u;
    r->data_stuck_mask = 0u;
    r->addr_stuck0_mask = 0u;
    r->stuck_cell = -1;
    r->calls = 0u;
}

int main(void)
{
    SdramBanks b;

    /* bank 2 not enabled: never touched, whatever the probe says */
    static Ram untouched;
    reset(&untouched);
    SdramBanks_Resolve(&b, 1, 0, 1, &OPS, &untouched, WORDS);
    CHECK(b.bank1 == 1u && b.bank2 == 0u && b.bank2_probed == 0u && b.any == 1u, "bank 1 only");
    CHECK(untouched.calls == 0u, "a bank that is not enabled is not touched");
    CHECK(b.bank2_report.status == MEMTEST_OK, "no probe, no finding");

    /* bank 2 enabled, probe off: taken on trust, not touched */
    SdramBanks_Resolve(&b, 1, 1, 0, &OPS, &untouched, WORDS);
    CHECK(b.bank1 == 1u && b.bank2 == 1u && b.bank2_probed == 0u && b.any == 1u, "bank 2 on trust with the probe off");
    CHECK(untouched.calls == 0u, "no probe, no access");

    /* probe on, good memory */
    static Ram ok;
    reset(&ok);
    SdramBanks_Resolve(&b, 1, 1, 1, &OPS, &ok, WORDS);
    CHECK(b.bank1 == 1u && b.bank2 == 1u && b.bank2_probed == 1u && b.any == 1u, "a good bank 2 stays on");
    CHECK(b.bank2_report.status == MEMTEST_OK && ok.calls > 0u, "the probe ran and passed");

    /* probe on, each kind of fault drops bank 2 and keeps bank 1 */
    static Ram data;
    reset(&data);
    data.data_stuck_mask = 0x1u;
    SdramBanks_Resolve(&b, 1, 1, 1, &OPS, &data, WORDS);
    CHECK(b.bank1 == 1u && b.bank2 == 0u && b.bank2_probed == 1u && b.any == 1u, "a stuck data line drops bank 2");
    CHECK(b.bank2_report.status == MEMTEST_FAIL_DATA_BUS, "reported as a data bus fault");

    static Ram addr;
    reset(&addr);
    addr.addr_stuck0_mask = 0x8u;
    SdramBanks_Resolve(&b, 1, 1, 1, &OPS, &addr, WORDS);
    CHECK(b.bank2 == 0u && b.bank1 == 1u && b.any == 1u, "a stuck address line drops bank 2");
    CHECK(b.bank2_report.status == MEMTEST_FAIL_ADDRESS_BUS, "reported as an address bus fault");

    static Ram cell;
    reset(&cell);
    cell.stuck_cell = 5;
    SdramBanks_Resolve(&b, 1, 1, 1, &OPS, &cell, WORDS);
    CHECK(b.bank2 == 0u && b.bank1 == 1u && b.any == 1u, "a stuck cell drops bank 2");
    CHECK(b.bank2_report.status == MEMTEST_FAIL_CELL && b.bank2_report.word_index == 5u, "reported as a cell fault at the cell");

    static Ram window;
    reset(&window);
    SdramBanks_Resolve(&b, 1, 1, 1, &OPS, &window, 3u);
    CHECK(b.bank2 == 0u && b.bank2_probed == 1u && b.bank2_report.status == MEMTEST_BAD_WINDOW, "a window that is not a power of two counts as a failed probe");

    /* bank 1 off: with bank 2 failing, nothing is left */
    static Ram dead;
    reset(&dead);
    dead.data_stuck_mask = 0x1u;
    SdramBanks_Resolve(&b, 0, 1, 1, &OPS, &dead, WORDS);
    CHECK(b.bank1 == 0u && b.bank2 == 0u && b.any == 0u, "no bank left: the firmware runs without external memory");
    static Ram alive;
    reset(&alive);
    SdramBanks_Resolve(&b, 0, 1, 1, &OPS, &alive, WORDS);
    CHECK(b.bank1 == 0u && b.bank2 == 1u && b.any == 1u, "bank 2 alone, when it passes");
    SdramBanks_Resolve(&b, 0, 0, 0, &OPS, &alive, WORDS);
    CHECK(b.any == 0u, "no bank enabled, none usable");

    /* the wrappers with the values of this build */
    static Ram built;
    reset(&built);
    SdramBanks_ResolveBuild(&b, &OPS, &built);
    CHECK(b.bank1 == HYDRA_SDRAM_BANK1_ENABLED, "bank 1 follows the profile");
    CHECK(b.bank2 == HYDRA_SDRAM_BANK2_ENABLED, "a good bank 2 follows the profile");
    CHECK(b.bank2_probed == (HYDRA_SDRAM_BANK2_ENABLED && HYDRA_SDRAM_BANK2_PROBE ? 1u : 0u), "the probe follows its switch");
    const uint32_t expected = (HYDRA_SDRAM_BANK1_ENABLED ? 134217728u : 0u) + (HYDRA_SDRAM_BANK2_ENABLED ? 134217728u : 0u);
    CHECK(SdramBanks_UsableBytes(&b) == expected, "the usable bytes are the windows of the banks that are on");
#if HYDRA_SDRAM_BANK2_ENABLED && HYDRA_SDRAM_BANK2_PROBE
    static Ram broken;
    reset(&broken);
    broken.data_stuck_mask = 0x1u;
    SdramBanks_ResolveBuild(&b, &OPS, &broken);
    CHECK(b.bank2 == 0u && SdramBanks_UsableBytes(&b) == (HYDRA_SDRAM_BANK1_ENABLED ? 134217728u : 0u), "a failing bank 2 leaves only bank 1");
#endif
#if !HYDRA_SDRAM_BANK2_PROBE
    CHECK(built.calls == 0u, "the probe ships off: the build does not touch bank 2");
#endif

    if (failures) { printf("sdram_banks: %d failure(s)\n", failures); return 1; }
    printf("sdram_banks: all checks passed\n");
    return 0;
}
