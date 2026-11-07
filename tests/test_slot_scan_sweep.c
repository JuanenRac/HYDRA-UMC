/* =============================================================================
 * HYDRA-UMC Firmware - host test of slot_scan_sweep.h (one full slot scan: eight windows)
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * The bus is a stand-in that replays the windows of tests/slot_scan_vectors.json (through tests/slot_scan_vectors.h): for each scenario the sweep has to give
 * the verdict of every slot that the Python reference gives. The order of the windows, the scan id handed to each and the codes that go to the SPI link are
 * checked on top of that.
 */
#include <stdio.h>
#include <string.h>
#include "slot_scan_sweep.h"
#include "slot_scan_vectors.h"

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } } while (0)

typedef struct {
    const char *scenario;
    uint8_t order[16];
    uint8_t calls;
    uint8_t scan_ids[16];
} Bus;

static void replay_window(void *context, uint8_t slot, uint8_t scan_id, SlotScanMaster *window)
{
    Bus *bus = (Bus *)context;
    if (bus->calls < 16u) {
        bus->order[bus->calls] = slot;
        bus->scan_ids[bus->calls] = scan_id;
    }
    bus->calls++;
    const unsigned count = sizeof slot_scan_vector_windows / sizeof slot_scan_vector_windows[0];
    for (unsigned i = 0; i < count; i++) {
        const SlotScanVectorWindow *w = &slot_scan_vector_windows[i];
        if (w->slot != slot || strcmp(w->name, bus->scenario) != 0) continue;
        for (uint8_t f = 0; f < w->nframes; f++) SlotScanMaster_OnFrame(window, w->frames[f], w->dlc[f]);
        for (uint16_t e = 0; e < w->bus_errors; e++) SlotScanMaster_OnBusError(window);
        return;
    }
}

static void test_every_scenario_gives_the_verdicts_of_the_reference(void)
{
    const unsigned count = sizeof slot_scan_vector_windows / sizeof slot_scan_vector_windows[0];
    unsigned scenarios = 0;
    for (unsigned i = 0; i < count; i++) {
        const SlotScanVectorWindow *first = &slot_scan_vector_windows[i];
        if (first->slot != 0u) continue; /* the rows of a scenario are slot 0 to 7 in a row */
        scenarios++;
        Bus bus;
        memset(&bus, 0, sizeof bus);
        bus.scenario = first->name;
        uint8_t verdicts[8];
        memset(verdicts, 0xEE, sizeof verdicts);
        SlotScanSweep_Run(replay_window, &bus, first->scan_id, verdicts);
        for (unsigned s = 0; s < 8; s++) {
            const SlotScanVectorWindow *row = &slot_scan_vector_windows[i + s];
            if (verdicts[s] != row->expect) printf("  %s, slot %u: expected %u got %u\n", first->name, s, row->expect, verdicts[s]);
            CHECK(verdicts[s] == row->expect, first->name);
        }
        CHECK(bus.calls == 8u, "one window per slot, no more");
        for (unsigned s = 0; s < 8; s++) {
            CHECK(bus.order[s] == s, "the windows run in the order of the slots");
            CHECK(bus.scan_ids[s] == first->scan_id, "every window carries the scan id of the request");
        }
    }
    CHECK(scenarios >= 9, "all the scenarios of the vectors were replayed");
}

static void test_a_sweep_without_a_bus_finds_nobody(void)
{
    uint8_t verdicts[8];
    memset(verdicts, 0xEE, sizeof verdicts);
    SlotScanSweep_Run(0, 0, 5, verdicts);
    for (unsigned s = 0; s < 8; s++) CHECK(verdicts[s] == SLOT_SCAN_ABSENT, "nothing heard is absent, never ok");
}

static void test_the_verdict_codes_are_those_of_the_spi_link(void)
{
    CHECK(SLOT_SCAN_ABSENT == 0 && SLOT_SCAN_OK == 1 && SLOT_SCAN_DUPLICATE == 2 && SLOT_SCAN_MISPLACED == 3 && SLOT_SCAN_NOISY == 4, "tools/spi_system.py VERDICTS");
}

static void only_slot_three_answers(void *context, uint8_t slot, uint8_t scan_id, SlotScanMaster *window)
{
    (void)context;
    if (slot != 3u) return;
    uint8_t frame[8] = {SLOT_SCAN_REPLY_MAGIC, 3, scan_id, 0x10, 0x20, 0x30, 0x40, 0};
    for (int i = 0; i < 7; i++) frame[7] ^= frame[i];
    SlotScanMaster_OnFrame(window, frame, 8);
}

static void test_each_window_starts_clean(void)
{
    uint8_t verdicts[8];
    SlotScanSweep_Run(only_slot_three_answers, 0, 9, verdicts);
    for (unsigned s = 0; s < 8; s++) CHECK(verdicts[s] == (s == 3u ? SLOT_SCAN_OK : SLOT_SCAN_ABSENT), "the answer of one slot never leaks into another window");
    /* the same sweep again gives the same result: nothing is kept between sweeps */
    uint8_t again[8];
    SlotScanSweep_Run(only_slot_three_answers, 0, 9, again);
    CHECK(memcmp(verdicts, again, 8) == 0, "a second sweep gives the same verdicts");
    /* a reply of another scan id is not counted */
    uint8_t other[8];
    SlotScanSweep_Run(only_slot_three_answers, 0, 10, other); /* the stand-in answers with the id it is given, so this is still ok */
    CHECK(other[3] == SLOT_SCAN_OK, "the stand-in answers the scan it is asked");
}

int main(void)
{
    test_every_scenario_gives_the_verdicts_of_the_reference();
    test_a_sweep_without_a_bus_finds_nobody();
    test_the_verdict_codes_are_those_of_the_spi_link();
    test_each_window_starts_clean();
    printf("slot_scan_sweep: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
