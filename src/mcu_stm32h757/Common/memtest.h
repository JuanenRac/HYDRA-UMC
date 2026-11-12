// =============================================================================
// HYDRA-UMC Kinematic Brain - memory tests for the external SDRAM (data bus, address bus, march), header-only and free of the HAL
// Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
// GPL-3.0-or-later - see repo root LICENSE
//
// The tests the board guide asks for before the external memory is used for the heap, a framebuffer or DMA: a walking-ones test of the data bus, a power-of-two test of the
// address bus and a march test of the cells. The memory is reached through two callbacks that take a word index (read and write of one 32-bit word), so the host test can
// run them against a RAM double with stuck data lines, stuck or shorted address lines and stuck cells, and on the board the callbacks are plain volatile accesses.
//
//   - Memtest_DataBus: walks a one and then a zero through all 32 data lines of one word; finds a data line that is stuck or shorted to its neighbour.
//   - Memtest_AddressBus: writes a pattern at every power-of-two word offset and checks that no write shows up at another offset; finds an address line that is stuck or shorted.
//   - Memtest_March: the March C- algorithm over a window of words (up w0; up r0 w1; up r1 w0; down r0 w1; down r1 w0; down r0); finds stuck cells, transition faults and
//     coupling between cells. It destroys the contents of the window.
//   - Memtest_Run: the three in this order, stopping at the first failure; the window is a number of words (a power of two) from the start of the memory.
// The temperature test of the guide is not here: it needs the board. Nothing here has run on a board.
// =============================================================================
#ifndef HYDRA_MEMTEST_H
#define HYDRA_MEMTEST_H

#include <stdint.h>

typedef struct {
    uint32_t (*read)(void *ctx, uint32_t word_index);
    void (*write)(void *ctx, uint32_t word_index, uint32_t value);
} MemOps;

typedef enum {
    MEMTEST_OK = 0,
    MEMTEST_FAIL_DATA_BUS = 1,
    MEMTEST_FAIL_ADDRESS_BUS = 2,
    MEMTEST_FAIL_CELL = 3,
    MEMTEST_BAD_WINDOW = 4
} MemtestStatus;

typedef struct {
    MemtestStatus status;
    uint32_t word_index;   /* where it failed */
    uint32_t expected;
    uint32_t actual;
    uint32_t element;      /* the element of the march (0..5) that failed */
} MemtestReport;

static inline void Memtest_Fail(MemtestReport *report, MemtestStatus status, uint32_t word_index, uint32_t expected, uint32_t actual, uint32_t element)
{
    report->status = status;
    report->word_index = word_index;
    report->expected = expected;
    report->actual = actual;
    report->element = element;
}

static inline MemtestStatus Memtest_DataBus(const MemOps *ops, void *ctx, uint32_t word_index, MemtestReport *report)
{
    for (uint32_t pattern = 1u; pattern != 0u; pattern <<= 1) {
        for (int inverse = 0; inverse < 2; inverse++) {
            const uint32_t value = inverse ? ~pattern : pattern;
            ops->write(ctx, word_index, value);
            const uint32_t got = ops->read(ctx, word_index);
            if (got != value) {
                Memtest_Fail(report, MEMTEST_FAIL_DATA_BUS, word_index, value, got, 0u);
                return MEMTEST_FAIL_DATA_BUS;
            }
        }
    }
    return MEMTEST_OK;
}

/* `words` is a power of two; offset 0 is the base. Checks that a write at one power-of-two offset does not appear at the base or at any other power-of-two offset. */
static inline MemtestStatus Memtest_AddressBus(const MemOps *ops, void *ctx, uint32_t words, MemtestReport *report)
{
    const uint32_t pattern = 0xAAAAAAAAu, anti = 0x55555555u;
    for (uint32_t offset = 1u; offset < words; offset <<= 1) ops->write(ctx, offset, pattern);
    ops->write(ctx, 0u, anti);
    /* an address line stuck at zero shows up as a pattern that was overwritten at the base: check every offset still holds the pattern */
    for (uint32_t offset = 1u; offset < words; offset <<= 1) {
        const uint32_t got = ops->read(ctx, offset);
        if (got != pattern) {
            Memtest_Fail(report, MEMTEST_FAIL_ADDRESS_BUS, offset, pattern, got, 0u);
            return MEMTEST_FAIL_ADDRESS_BUS;
        }
    }
    ops->write(ctx, 0u, pattern);
    for (uint32_t offset = 1u; offset < words; offset <<= 1) {
        ops->write(ctx, offset, anti);
        uint32_t got = ops->read(ctx, 0u);
        if (got != pattern) {
            Memtest_Fail(report, MEMTEST_FAIL_ADDRESS_BUS, 0u, pattern, got, 0u);
            return MEMTEST_FAIL_ADDRESS_BUS;
        }
        for (uint32_t other = 1u; other < words; other <<= 1) {
            if (other == offset) continue;
            got = ops->read(ctx, other);
            if (got != pattern) {
                Memtest_Fail(report, MEMTEST_FAIL_ADDRESS_BUS, other, pattern, got, 0u);
                return MEMTEST_FAIL_ADDRESS_BUS;
            }
        }
        ops->write(ctx, offset, pattern);
    }
    return MEMTEST_OK;
}

static inline MemtestStatus Memtest_March(const MemOps *ops, void *ctx, uint32_t words, MemtestReport *report)
{
    const uint32_t zeros = 0u, ones = 0xFFFFFFFFu;
    uint32_t got;
    for (uint32_t i = 0; i < words; i++) ops->write(ctx, i, zeros);                                                    /* up(w0) */
    for (uint32_t i = 0; i < words; i++) {                                                                              /* up(r0, w1) */
        if ((got = ops->read(ctx, i)) != zeros) { Memtest_Fail(report, MEMTEST_FAIL_CELL, i, zeros, got, 1u); return MEMTEST_FAIL_CELL; }
        ops->write(ctx, i, ones);
    }
    for (uint32_t i = 0; i < words; i++) {                                                                              /* up(r1, w0) */
        if ((got = ops->read(ctx, i)) != ones) { Memtest_Fail(report, MEMTEST_FAIL_CELL, i, ones, got, 2u); return MEMTEST_FAIL_CELL; }
        ops->write(ctx, i, zeros);
    }
    for (uint32_t i = words; i-- > 0u;) {                                                                               /* down(r0, w1) */
        if ((got = ops->read(ctx, i)) != zeros) { Memtest_Fail(report, MEMTEST_FAIL_CELL, i, zeros, got, 3u); return MEMTEST_FAIL_CELL; }
        ops->write(ctx, i, ones);
    }
    for (uint32_t i = words; i-- > 0u;) {                                                                               /* down(r1, w0) */
        if ((got = ops->read(ctx, i)) != ones) { Memtest_Fail(report, MEMTEST_FAIL_CELL, i, ones, got, 4u); return MEMTEST_FAIL_CELL; }
        ops->write(ctx, i, zeros);
    }
    for (uint32_t i = words; i-- > 0u;) {                                                                               /* down(r0) */
        if ((got = ops->read(ctx, i)) != zeros) { Memtest_Fail(report, MEMTEST_FAIL_CELL, i, zeros, got, 5u); return MEMTEST_FAIL_CELL; }
    }
    return MEMTEST_OK;
}

/* `words` must be a power of two and at least 2. The report is filled on a failure; on success its status is MEMTEST_OK. */
static inline MemtestStatus Memtest_Run(const MemOps *ops, void *ctx, uint32_t words, MemtestReport *report)
{
    report->status = MEMTEST_OK;
    report->word_index = report->expected = report->actual = report->element = 0u;
    if (words < 2u || (words & (words - 1u)) != 0u) {
        report->status = MEMTEST_BAD_WINDOW;
        return MEMTEST_BAD_WINDOW;
    }
    MemtestStatus status = Memtest_DataBus(ops, ctx, 0u, report);
    if (status != MEMTEST_OK) return status;
    status = Memtest_AddressBus(ops, ctx, words, report);
    if (status != MEMTEST_OK) return status;
    return Memtest_March(ops, ctx, words, report);
}

#endif // HYDRA_MEMTEST_H
