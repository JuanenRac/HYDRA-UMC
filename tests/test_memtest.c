/* =============================================================================
 * HYDRA-UMC Firmware - host test of memtest.h (data bus, address bus and march tests for the external SDRAM)
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * The memory is a double with faults: a data line stuck at 0 or 1, two data lines bridged, an address line stuck at 0 or 1, two address lines bridged, a cell with a bit
 * stuck, a cell that cannot go from 1 to 0 and a write that disturbs the next cell. Each fault must be found by the test that exists for it, and a good memory must pass.
 * Nothing here touches a memory controller.
 */
#include <stdio.h>
#include <string.h>
#include "memtest.h"

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } } while (0)

#define WORDS 4096u

typedef struct {
    uint32_t cell[WORDS];
    uint32_t data_stuck_mask, data_stuck_value;      /* data lines stuck at a value on read */
    int data_bridge_a, data_bridge_b;                /* two data lines wired together (-1: none): a one on either shows on both */
    uint32_t addr_stuck0_mask, addr_stuck1_mask;     /* address lines stuck at 0 or at 1 */
    int addr_bridge_from, addr_bridge_to;            /* address line `to` follows line `from` (-1: none) */
    int stuck_cell; uint32_t stuck_cell_mask, stuck_cell_value;
    int no_fall_cell;                                /* this cell cannot go from 1 to 0 (-1: none) */
    int disturb_cell;                                /* a write to this cell sets bit 0 of the next one (-1: none) */
} Ram;

static uint32_t map_index(const Ram *r, uint32_t i)
{
    i &= ~r->addr_stuck0_mask;
    i |= r->addr_stuck1_mask;
    if (r->addr_bridge_from >= 0) {
        i &= ~(1u << r->addr_bridge_to);
        i |= ((i >> r->addr_bridge_from) & 1u) << r->addr_bridge_to;
    }
    return i % WORDS;
}

static void ram_write(void *ctx, uint32_t index, uint32_t value)
{
    Ram *r = (Ram *)ctx;
    const uint32_t at = map_index(r, index);
    if (r->data_bridge_a >= 0) {
        const uint32_t either = ((value >> r->data_bridge_a) | (value >> r->data_bridge_b)) & 1u;
        value = (value & ~((1u << r->data_bridge_a) | (1u << r->data_bridge_b))) | (either << r->data_bridge_a) | (either << r->data_bridge_b);
    }
    if ((int)at == r->no_fall_cell) value |= r->cell[at];
    r->cell[at] = value;
    if ((int)at == r->disturb_cell && at + 1u < WORDS) r->cell[at + 1u] |= 1u;
}

static uint32_t ram_read(void *ctx, uint32_t index)
{
    Ram *r = (Ram *)ctx;
    const uint32_t at = map_index(r, index);
    uint32_t value = r->cell[at];
    if ((int)at == r->stuck_cell) value = (value & ~r->stuck_cell_mask) | (r->stuck_cell_value & r->stuck_cell_mask);
    return (value & ~r->data_stuck_mask) | (r->data_stuck_value & r->data_stuck_mask);
}

static const MemOps OPS = {ram_read, ram_write};

static void ram_init(Ram *r)
{
    memset(r, 0, sizeof *r);
    r->data_bridge_a = r->data_bridge_b = -1;
    r->addr_bridge_from = r->addr_bridge_to = -1;
    r->stuck_cell = r->no_fall_cell = r->disturb_cell = -1;
}

int main(void)
{
    static Ram ram;
    MemtestReport report;

    ram_init(&ram);
    CHECK(Memtest_Run(&OPS, &ram, WORDS, &report) == MEMTEST_OK && report.status == MEMTEST_OK, "a good memory passes the three tests");
    ram_init(&ram);
    CHECK(Memtest_Run(&OPS, &ram, 2u, &report) == MEMTEST_OK, "the smallest window passes");

    /* a window that is not a power of two is refused before anything is written */
    ram_init(&ram);
    ram.cell[0] = 0x1234u;
    CHECK(Memtest_Run(&OPS, &ram, 3u, &report) == MEMTEST_BAD_WINDOW && report.status == MEMTEST_BAD_WINDOW, "three words: bad window");
    CHECK(Memtest_Run(&OPS, &ram, 1u, &report) == MEMTEST_BAD_WINDOW, "one word: bad window");
    CHECK(Memtest_Run(&OPS, &ram, 0u, &report) == MEMTEST_BAD_WINDOW, "no words: bad window");
    CHECK(ram.cell[0] == 0x1234u, "a refused window writes nothing");

    /* data bus faults are found by the data bus test, with the pattern that shows them */
    ram_init(&ram);
    ram.data_stuck_mask = 1u << 7; ram.data_stuck_value = 0;
    CHECK(Memtest_Run(&OPS, &ram, WORDS, &report) == MEMTEST_FAIL_DATA_BUS && (report.expected ^ report.actual) == (1u << 7), "data line 7 stuck at 0: the wrong bit is bit 7");
    ram_init(&ram);
    ram.data_stuck_mask = 1u << 31; ram.data_stuck_value = 1u << 31;
    CHECK(Memtest_Run(&OPS, &ram, WORDS, &report) == MEMTEST_FAIL_DATA_BUS && (report.expected ^ report.actual) == (1u << 31), "data line 31 stuck at 1: the wrong bit is bit 31");
    ram_init(&ram);
    ram.data_bridge_a = 3; ram.data_bridge_b = 4;
    CHECK(Memtest_Run(&OPS, &ram, WORDS, &report) == MEMTEST_FAIL_DATA_BUS && report.actual != report.expected, "data lines 3 and 4 bridged");

    /* address bus faults are found by the address bus test */
    for (unsigned bit = 0; bit < 12u; bit++) {
        ram_init(&ram);
        ram.addr_stuck0_mask = 1u << bit;
        CHECK(Memtest_Run(&OPS, &ram, WORDS, &report) == MEMTEST_FAIL_ADDRESS_BUS, "an address line stuck at 0 is found");
        ram_init(&ram);
        ram.addr_stuck1_mask = 1u << bit;
        CHECK(Memtest_Run(&OPS, &ram, WORDS, &report) == MEMTEST_FAIL_ADDRESS_BUS, "an address line stuck at 1 is found");
    }
    ram_init(&ram);
    ram.addr_bridge_from = 3; ram.addr_bridge_to = 4;
    CHECK(Memtest_Run(&OPS, &ram, WORDS, &report) == MEMTEST_FAIL_ADDRESS_BUS, "address lines 3 and 4 bridged");

    /* cell faults are found by the march, with the element and the cell */
    ram_init(&ram);
    ram.stuck_cell = 1000; ram.stuck_cell_mask = 1u << 5; ram.stuck_cell_value = 1u << 5;
    CHECK(Memtest_Run(&OPS, &ram, WORDS, &report) == MEMTEST_FAIL_CELL && report.word_index == 1000u && report.element == 1u, "a bit stuck at 1 in cell 1000: element 1");
    ram_init(&ram);
    ram.stuck_cell = 2000; ram.stuck_cell_mask = 1u << 9; ram.stuck_cell_value = 0;
    CHECK(Memtest_Run(&OPS, &ram, WORDS, &report) == MEMTEST_FAIL_CELL && report.word_index == 2000u && report.element == 2u, "a bit stuck at 0 in cell 2000: element 2");
    ram_init(&ram);
    ram.no_fall_cell = 77;
    CHECK(Memtest_Run(&OPS, &ram, WORDS, &report) == MEMTEST_FAIL_CELL && report.word_index == 77u, "a cell that cannot fall from 1 to 0");
    ram_init(&ram);
    ram.disturb_cell = 200;
    CHECK(Memtest_Run(&OPS, &ram, WORDS, &report) == MEMTEST_FAIL_CELL && report.word_index == 201u, "a write that disturbs the next cell");

    /* the tests run on their own too */
    ram_init(&ram);
    ram.stuck_cell = 5; ram.stuck_cell_mask = 1u; ram.stuck_cell_value = 1u;
    CHECK(Memtest_DataBus(&OPS, &ram, 0u, &report) == MEMTEST_OK, "the data bus test does not see a bad cell elsewhere");
    CHECK(Memtest_AddressBus(&OPS, &ram, WORDS, &report) == MEMTEST_OK, "nor does the address bus test");
    CHECK(Memtest_March(&OPS, &ram, WORDS, &report) == MEMTEST_FAIL_CELL, "the march does");

    if (failures == 0) printf("MEMTEST=PASS\n");
    return failures == 0 ? 0 : 1;
}
