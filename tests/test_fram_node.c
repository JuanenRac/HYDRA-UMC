/* =============================================================================
 * HYDRA-UMC Firmware - host test of fram_node.h (the F-RAM erase and self-test requests of a controller board)
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * The chip is simulated by an array; nothing here has run on a board. Built once with the switch off and once with it on.
 */
#include <stdio.h>
#include <string.h>
#include "fram_node.h"

#ifndef FRAM_TEST_SWITCH
#define FRAM_TEST_SWITCH 0
#endif

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } } while (0)

#define CHIP_SIZE 300u
/* with the journal on, the copy and its mark sit under the scratch area and are rewritten by a self-test */
#if HYDRA_FRAM_SELFTEST_JOURNAL
#define KEPT_BYTES (CHIP_SIZE - FRAM_JOURNAL_RESERVED)
#else
#define KEPT_BYTES CHIP_SIZE
#endif
typedef struct {
    uint8_t cells[CHIP_SIZE];
    int fail_write_at; /* address from which a write fails, -1 never */
    int stuck_at;      /* address that always reads 0xFF, -1 none */
    int fail_read_at;  /* a read that starts at this address fails, -1 never */
    unsigned writes;
} SimChip;

static int sim_read(void *context, uint32_t address, uint8_t *data, uint32_t length)
{
    SimChip *c = (SimChip *)context;
    if (address + length > CHIP_SIZE || (int)address == c->fail_read_at) return 1;
    for (uint32_t i = 0; i < length; i++) data[i] = ((int)(address + i) == c->stuck_at) ? 0xFFu : c->cells[address + i];
    return 0;
}

static int sim_write(void *context, uint32_t address, const uint8_t *data, uint32_t length)
{
    SimChip *c = (SimChip *)context;
    if (address + length > CHIP_SIZE) return 1;
    if (c->fail_write_at >= 0 && (int)address >= c->fail_write_at) return 1;
    for (uint32_t i = 0; i < length; i++) c->cells[address + i] = data[i];
    c->writes++;
    return 0;
}

static void fill(SimChip *c)
{
    for (uint32_t i = 0; i < CHIP_SIZE; i++) c->cells[i] = (uint8_t)(0x30u + i);
    c->fail_write_at = -1;
    c->stuck_at = -1;
    c->fail_read_at = -1;
    c->writes = 0;
}

static FramNode_t node_for(SimChip *c, FramDriver_t *d, uint8_t enabled)
{
    FramNode_t n;
    d->size = CHIP_SIZE;
    d->read = sim_read;
    d->write = sim_write;
    d->context = c;
    FramNode_Init(&n, d);
    n.enabled = enabled;
    return n;
}

static void test_the_switch_defaults(void)
{
    FramNode_t n;
    FramNode_Init(&n, NULL);
    CHECK(n.enabled == (FRAM_TEST_SWITCH ? 1u : 0u), "the switch is the build switch");
    CHECK(n.driver == NULL, "no driver until the chip is fixed");
}

static void test_frames(void)
{
    uint8_t req[8], op = 0, slot = 0;
    Fram_BuildRequest(req, FRAM_OP_ERASE, 5);
    CHECK(req[0] == 0xFA && req[1] == 1 && req[2] == 5 && req[3] == 'E' && req[6] == 'S', "erase request layout");
    CHECK(Fram_DecodeRequest(req, 8, &op, &slot) && op == FRAM_OP_ERASE && slot == 5, "erase request decodes");
    Fram_BuildRequest(req, FRAM_OP_SELFTEST, 2);
    CHECK(req[3] == 'T' && req[6] == 'T', "self-test request layout");
    CHECK(Fram_DecodeRequest(req, 8, &op, &slot) && op == FRAM_OP_SELFTEST && slot == 2, "self-test request decodes");
    CHECK(!Fram_DecodeRequest(req, 7, &op, &slot), "short request");
    CHECK(!Fram_DecodeRequest(req, 0, &op, &slot), "empty request");
    for (int byte = 0; byte < 8; byte++) {
        uint8_t bad[8];
        memcpy(bad, req, 8);
        bad[byte] ^= 0x01u;
        CHECK(!Fram_DecodeRequest(bad, 8, &op, &slot), "one flipped bit in any byte is refused");
    }
    Fram_BuildRequest(req, FRAM_OP_ERASE, 0);
    req[1] = FRAM_OP_SELFTEST; /* the word of the other operation is missing */
    req[7] = Fram_Check(req);
    CHECK(!Fram_DecodeRequest(req, 8, &op, &slot), "the word must be the one of the operation");
    req[1] = 9; req[7] = Fram_Check(req);
    CHECK(!Fram_DecodeRequest(req, 8, &op, &slot), "unknown operation");
    uint8_t resp[8];
    Fram_BuildResponse(resp, FRAM_OP_ERASE, FRAM_STATUS_OK, 3, 0x01020304u);
    CHECK(resp[0] == 0xFB && resp[2] == 0 && resp[3] == 3 && resp[4] == 1 && resp[5] == 2 && resp[6] == 3 && resp[7] == 4, "response layout");
    CHECK(!Fram_DecodeRequest(resp, 8, &op, &slot), "a response is never taken for a request");
}

static void test_refusals(void)
{
    SimChip chip; FramDriver_t d; uint8_t req[8], out[8];
    fill(&chip);
    FramNode_t off = node_for(&chip, &d, 0);
    Fram_BuildRequest(req, FRAM_OP_ERASE, 4);
    CHECK(FramNode_Handle(&off, 4, 0, req, 8, out) == 1 && out[2] == FRAM_STATUS_DISABLED, "switch off answers DISABLED");
    CHECK(chip.writes == 0 && chip.cells[0] == 0x30, "switch off touches nothing");
    FramNode_t on = node_for(&chip, &d, 1);
    CHECK(FramNode_Handle(&on, 3, 0, req, 8, out) == 1 && out[2] == FRAM_STATUS_WRONG_SLOT, "another slot");
    CHECK(FramNode_Handle(&on, 4, 1, req, 8, out) == 1 && out[2] == FRAM_STATUS_BUSY, "an update is running");
    CHECK(chip.writes == 0, "refusals touch nothing");
    req[0] = 0;
    CHECK(FramNode_Handle(&on, 4, 0, req, 8, out) == 0, "noise gets no answer");
    FramNode_t nochip; FramNode_Init(&nochip, NULL); nochip.enabled = 1;
    Fram_BuildRequest(req, FRAM_OP_ERASE, 4);
    CHECK(FramNode_Handle(&nochip, 4, 0, req, 8, out) == 1 && out[2] == FRAM_STATUS_NO_CHIP, "no driver answers NO_CHIP");
    FramDriver_t empty = {0, sim_read, sim_write, &chip};
    nochip.driver = &empty;
    CHECK(FramNode_Handle(&nochip, 4, 0, req, 8, out) == 1 && out[2] == FRAM_STATUS_NO_CHIP, "a driver with no size answers NO_CHIP");
    FramDriver_t half = {CHIP_SIZE, sim_read, NULL, &chip};
    nochip.driver = &half;
    CHECK(FramNode_Handle(&nochip, 4, 0, req, 8, out) == 1 && out[2] == FRAM_STATUS_NO_CHIP, "a driver with no write answers NO_CHIP");
}

static void test_erase(void)
{
    SimChip chip; FramDriver_t d; uint8_t req[8], out[8];
    fill(&chip);
    FramNode_t n = node_for(&chip, &d, 1);
    Fram_BuildRequest(req, FRAM_OP_ERASE, 6);
    CHECK(FramNode_Handle(&n, 6, 0, req, 8, out) == 1, "answered");
    CHECK(out[1] == FRAM_OP_ERASE && out[2] == FRAM_STATUS_OK && out[3] == 6, "erase ok, board id in the detail");
    CHECK(out[4] == 0 && out[5] == 0 && out[6] == 1 && out[7] == (CHIP_SIZE & 0xFFu), "all the bytes written (a size that is not a multiple of the chunk)");
    int all_zero = 1;
    for (uint32_t i = 0; i < CHIP_SIZE; i++) if (chip.cells[i] != 0) all_zero = 0;
    CHECK(all_zero, "every cell is zero");
    /* a write that fails half way */
    fill(&chip); chip.fail_write_at = 100;
    CHECK(FramNode_Handle(&n, 6, 0, req, 8, out) == 1 && out[2] == FRAM_STATUS_CHIP_ERROR, "write failure");
    CHECK(out[7] == 128 && out[6] == 0, "written count stops at the last chunk that went in");
    /* a cell that does not take the zero */
    fill(&chip); chip.stuck_at = 250;
    CHECK(FramNode_Handle(&n, 6, 0, req, 8, out) == 1 && out[2] == FRAM_STATUS_VERIFY_FAIL, "readback differs");
    CHECK(out[6] == 0 && out[7] == 250, "the failing address is reported");
}

static void test_selftest(void)
{
    SimChip chip; FramDriver_t d; uint8_t req[8], out[8], before[CHIP_SIZE];
    fill(&chip);
    memcpy(before, chip.cells, CHIP_SIZE);
    FramNode_t n = node_for(&chip, &d, 1);
    Fram_BuildRequest(req, FRAM_OP_SELFTEST, 1);
    CHECK(FramNode_Handle(&n, 1, 0, req, 8, out) == 1 && out[1] == FRAM_OP_SELFTEST && out[2] == FRAM_STATUS_OK, "self-test ok");
    CHECK(memcmp(before, chip.cells, KEPT_BYTES) == 0 && memcmp(&before[CHIP_SIZE - 64u], &chip.cells[CHIP_SIZE - 64u], 64u) == 0, "the self-test leaves the chip as it was");
    fill(&chip); chip.stuck_at = CHIP_SIZE - 10;
    memcpy(before, chip.cells, CHIP_SIZE);
    CHECK(FramNode_Handle(&n, 1, 0, req, 8, out) == 1 && out[2] == FRAM_STATUS_VERIFY_FAIL, "a stuck cell fails the test");
    CHECK(out[6] == 1 && out[7] == (uint8_t)(CHIP_SIZE - 10), "the stuck address");
    {
        int same = 1;
        for (uint32_t i = 0; i < CHIP_SIZE; i++) if (i != CHIP_SIZE - 10 && (i < KEPT_BYTES || i >= CHIP_SIZE - 64u) && chip.cells[i] != before[i]) same = 0;
        CHECK(same, "the area is put back even when the test fails (but for the cell that cannot be read)");
    }
    fill(&chip); chip.fail_write_at = 0;
    CHECK(FramNode_Handle(&n, 1, 0, req, 8, out) == 1 && out[2] == FRAM_STATUS_CHIP_ERROR, "a chip that takes no write");
    FramDriver_t tiny = {16, sim_read, sim_write, &chip};
    n.driver = &tiny;
    CHECK(FramNode_Handle(&n, 1, 0, req, 8, out) == 1 && out[2] == FRAM_STATUS_CHIP_ERROR, "a chip smaller than the scratch area");
}

static void test_seeded_sweep(void)
{
    SimChip chip; FramDriver_t d; uint8_t req[8], out[8];
    uint32_t state = 0x1234ABCDu;
    FramNode_t n = node_for(&chip, &d, 1);
    for (int round = 0; round < 500; round++) {
        uint8_t frame[8];
        for (int i = 0; i < 8; i++) {
            state = state * 1664525u + 1013904223u;
            frame[i] = (uint8_t)(state >> 24);
        }
        fill(&chip);
        uint8_t answered = FramNode_Handle(&n, 3, 0, frame, 8, out);
        CHECK(answered == 0 && chip.writes == 0, "random bytes are not a request and touch nothing");
    }
    Fram_BuildRequest(req, FRAM_OP_ERASE, 3);
    fill(&chip);
    CHECK(FramNode_Handle(&n, 3, 0, req, 8, out) == 1 && out[2] == 0, "and a real one still works afterwards");
}

#if HYDRA_FRAM_SELFTEST_JOURNAL
/* The twin of a reset: after `budget` bytes written the power is gone (the write in progress is cut there and every later access fails). */
typedef struct {
    SimChip *chip;
    uint32_t budget;
} CutChip;

static int cut_read(void *context, uint32_t address, uint8_t *data, uint32_t length)
{
    CutChip *cut = (CutChip *)context;
    return cut->budget == 0u ? 1 : sim_read(cut->chip, address, data, length);
}

static int cut_write(void *context, uint32_t address, const uint8_t *data, uint32_t length)
{
    CutChip *cut = (CutChip *)context;
    uint32_t n = length < cut->budget ? length : cut->budget;
    if (address + length > CHIP_SIZE) return 1;
    for (uint32_t i = 0; i < n; i++) cut->chip->cells[address + i] = data[i];
    cut->budget -= n;
    return n == length && cut->budget > 0u ? 0 : 1;
}

static void test_a_reset_at_any_byte_keeps_the_scratch_area(void)
{
    const uint32_t scratch = CHIP_SIZE - FRAM_SELFTEST_BYTES;
    uint8_t before[FRAM_SELFTEST_BYTES];
    for (uint32_t budget = 1u; budget < 400u; budget++) {
        SimChip chip; FramDriver_t good; uint32_t failed_at = 0;
        fill(&chip);
        memcpy(before, &chip.cells[scratch], FRAM_SELFTEST_BYTES);
        CutChip cut = { &chip, budget };
        FramDriver_t d = { CHIP_SIZE, cut_read, cut_write, &cut };
        (void)FramNode_SelfTest(&d, &failed_at);
        FramNode_t after = node_for(&chip, &good, 1); /* the next start */
        CHECK(FramNode_Recover(&after) == FRAM_STATUS_OK, "recovery works on a good chip");
        CHECK(memcmp(before, &chip.cells[scratch], FRAM_SELFTEST_BYTES) == 0, "the scratch area is the one of before the test");
    }
}

static void test_recovery_leaves_other_things_alone(void)
{
    SimChip chip; FramDriver_t d; uint8_t before[CHIP_SIZE];
    fill(&chip);
    memcpy(before, chip.cells, CHIP_SIZE);
    FramNode_t n = node_for(&chip, &d, 1);
    CHECK(FramNode_Recover(&n) == FRAM_STATUS_OK && memcmp(before, chip.cells, CHIP_SIZE) == 0, "with no pending copy nothing is written");
    n.enabled = 0;
    CHECK(FramNode_Recover(&n) == FRAM_STATUS_DISABLED && chip.writes == 0, "the switch of the erase off: nothing is touched");
}
static void test_a_chip_that_fails_keeps_the_pending_copy(void)
{
    const uint32_t scratch = CHIP_SIZE - FRAM_SELFTEST_BYTES;
    const uint32_t journal = scratch - FRAM_SELFTEST_BYTES - FRAM_JOURNAL_MARK_BYTES;
    SimChip chip; FramDriver_t d; uint32_t failed_at = 0; uint8_t before[FRAM_SELFTEST_BYTES];
    fill(&chip);
    memcpy(before, &chip.cells[scratch], FRAM_SELFTEST_BYTES);
    FramNode_t n = node_for(&chip, &d, 1);
    chip.fail_write_at = (int)scratch; /* the patterns and the way back fail: the closed copy stays */
    CHECK(FramNode_SelfTest(&d, &failed_at) == FRAM_STATUS_CHIP_ERROR, "a chip that refuses the scratch area");
    CHECK(FramNode_Recover(&n) == FRAM_STATUS_CHIP_ERROR, "the way back fails while the chip refuses it");
    CHECK(FramNode_SelfTest(&d, &failed_at) == FRAM_STATUS_CHIP_ERROR, "and a new test does not overwrite the pending copy");
    chip.fail_write_at = -1;
    chip.fail_read_at = (int)journal + 64; /* the mark cannot be read */
    CHECK(FramNode_Recover(&n) == FRAM_STATUS_CHIP_ERROR, "a mark that cannot be read");
    chip.fail_read_at = (int)journal; /* the copy cannot be read */
    CHECK(FramNode_Recover(&n) == FRAM_STATUS_CHIP_ERROR, "a copy that cannot be read");
    chip.fail_read_at = -1;
    memset(&chip.cells[scratch], 0, FRAM_SELFTEST_BYTES);
    CHECK(FramNode_Recover(&n) == FRAM_STATUS_OK && memcmp(before, &chip.cells[scratch], FRAM_SELFTEST_BYTES) == 0, "the copy comes back once the chip answers");
    FramDriver_t small = {100u, sim_read, sim_write, &chip};
    CHECK(FramNode_SelfTest(&small, &failed_at) == FRAM_STATUS_CHIP_ERROR, "a chip without room for the journal");
}
#endif

int main(void)
{
    test_the_switch_defaults();
#if HYDRA_FRAM_SELFTEST_JOURNAL
    test_a_reset_at_any_byte_keeps_the_scratch_area();
    test_recovery_leaves_other_things_alone();
    test_a_chip_that_fails_keeps_the_pending_copy();
#endif
    test_frames();
    test_refusals();
    test_erase();
    test_selftest();
    test_seeded_sweep();
    if (failures) {
        printf("test_fram_node: %d FAILED\n", failures);
        return 1;
    }
    printf("test_fram_node: all passed\n");
    return 0;
}
