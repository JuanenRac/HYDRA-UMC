/* =============================================================================
 * HYDRA-UMC Firmware - host test of slot_scan_master.h (the whole slot scan on the master side)
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * The windows are the scenarios of tests/slot_scan_vectors.json read through tests/slot_scan_vectors.h (tools/gen_node_vectors.py): the same
 * list the Python reference (tools/slot_scan.py) is tested against, so both give the same verdict for every slot of every scenario. On top of that:
 * the probe equals the vectors, the reply decoder refuses what the vectors call malformed, the constants equal those of the board side
 * (slot_scan_node.h), and a seeded fuzz of random windows is checked against an independent model of the rules.
 */
#include <stdio.h>
#include <string.h>
#include "slot_scan_node.h"
#include "slot_scan_master.h"
#include "slot_scan_vectors.h"

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } } while (0)

static void test_constants_equal_the_board_side(void)
{
    CHECK(SLOT_SCAN_PROBE_MAGIC == 0x5Cu && SLOT_SCAN_REPLY_MAGIC == 0x5Du, "magics");
    CHECK(SLOT_SCAN_MASTER_SLOTS == 8u, "eight slots");
    CHECK(SLOT_SCAN_ROLE_BOOTLOADER == 0u && SLOT_SCAN_ROLE_APPLICATION == 1u, "roles of the board side");
}

static void test_probe_equals_the_vectors(void)
{
    for (unsigned i = 0; i < sizeof slot_scan_vector_probes / sizeof slot_scan_vector_probes[0]; i++) {
        const SlotScanVectorProbe *v = &slot_scan_vector_probes[i];
        uint8_t out[8];
        SlotScanMaster_EncodeProbe(v->scan_id, out);
        CHECK(memcmp(out, v->data, 8) == 0, v->name);
        uint8_t scan_id = 0xEE;
        CHECK(SlotScan_DecodeProbe(out, 8, &scan_id) == 1 && scan_id == v->scan_id, "the board side reads the probe of the master");
    }
    for (unsigned id = 0; id < 256; id++) {
        uint8_t out[8], scan_id = 0;
        SlotScanMaster_EncodeProbe((uint8_t)id, out);
        CHECK(SlotScan_DecodeProbe(out, 8, &scan_id) == 1 && scan_id == id, "every scan id round-trips through the board side");
    }
}

static void test_replies_equal_the_vectors(void)
{
    for (unsigned i = 0; i < sizeof slot_scan_vector_replies / sizeof slot_scan_vector_replies[0]; i++) {
        const SlotScanVectorReply *v = &slot_scan_vector_replies[i];
        uint8_t board = 0xEE, role = 0xEE, scan = 0xEE;
        uint32_t uid = 0;
        CHECK(SlotScanMaster_DecodeReply(v->data, 8, &board, &role, &scan, &uid) == 1, v->name);
        CHECK(board == v->board_id && role == v->role && scan == v->scan_id && uid == v->uid32, v->name);
    }
}

static void test_malformed_frames_are_refused(void)
{
    for (unsigned i = 0; i < sizeof slot_scan_vector_malformed / sizeof slot_scan_vector_malformed[0]; i++) {
        const SlotScanVectorMalformed *v = &slot_scan_vector_malformed[i];
        uint8_t board, role, scan;
        uint32_t uid;
        CHECK(SlotScanMaster_DecodeReply(v->data, v->dlc, &board, &role, &scan, &uid) == 0, v->name);
        SlotScanMaster m;
        SlotScanMaster_Begin(&m, 3, 7);
        CHECK(SlotScanMaster_OnFrame(&m, v->data, v->dlc) == 0, "a malformed frame does not count");
        CHECK(SlotScanMaster_Verdict(&m) == SLOT_SCAN_ABSENT, "and leaves the slot absent");
    }
    /* every single-bit flip of a good reply breaks the XOR check, the magic or a range, so none of them is read as a reply of the same board */
    const SlotScanVectorReply *good = &slot_scan_vector_replies[1];
    for (unsigned byte = 0; byte < 8; byte++) {
        for (unsigned bit = 0; bit < 8; bit++) {
            uint8_t frame[8], board, role, scan;
            uint32_t uid;
            memcpy(frame, good->data, 8);
            frame[byte] ^= (uint8_t)(1u << bit);
            if (SlotScanMaster_DecodeReply(frame, 8, &board, &role, &scan, &uid)) {
                CHECK(0, "a frame with one bit flipped was accepted");
            }
        }
    }
    /* length: only 8 bytes */
    uint8_t board, role, scan;
    uint32_t uid;
    for (uint8_t dlc = 0; dlc < 16; dlc++) {
        if (dlc == 8) continue;
        CHECK(SlotScanMaster_DecodeReply(good->data, dlc, &board, &role, &scan, &uid) == 0, "only a frame of 8 bytes is a reply");
    }
}

static void test_scenarios_of_the_reference_give_the_same_verdicts(void)
{
    unsigned count = sizeof slot_scan_vector_windows / sizeof slot_scan_vector_windows[0];
    CHECK(count >= 72, "the scenarios of the vectors are all there");
    for (unsigned i = 0; i < count; i++) {
        const SlotScanVectorWindow *w = &slot_scan_vector_windows[i];
        SlotScanMaster m;
        SlotScanMaster_Begin(&m, w->slot, w->scan_id);
        for (uint8_t f = 0; f < w->nframes; f++) SlotScanMaster_OnFrame(&m, w->frames[f], w->dlc[f]);
        for (uint16_t e = 0; e < w->bus_errors; e++) SlotScanMaster_OnBusError(&m);
        SlotScanVerdict got = SlotScanMaster_Verdict(&m);
        if ((int)got != (int)w->expect) printf("  window %u (%s, slot %u): expected %d got %d\n", i, w->name, w->slot, w->expect, (int)got);
        CHECK((int)got == (int)w->expect, w->name);
        CHECK(SlotScanMaster_WriteAllowed(got) == (w->expect == 1 ? 1u : 0u), "a write is allowed only on OK");
    }
}

static void test_the_order_of_the_frames_does_not_change_the_verdict(void)
{
    const SlotScanVectorWindow *dup = NULL;
    unsigned count = sizeof slot_scan_vector_windows / sizeof slot_scan_vector_windows[0];
    for (unsigned i = 0; i < count; i++) if (slot_scan_vector_windows[i].nframes == 2 && slot_scan_vector_windows[i].expect == 2 && slot_scan_vector_windows[i].bus_errors == 0) { dup = &slot_scan_vector_windows[i]; break; }
    CHECK(dup != NULL, "there is a window with two boards in the vectors");
    if (!dup) return;
    SlotScanMaster a, b;
    SlotScanMaster_Begin(&a, dup->slot, dup->scan_id);
    SlotScanMaster_Begin(&b, dup->slot, dup->scan_id);
    SlotScanMaster_OnFrame(&a, dup->frames[0], dup->dlc[0]);
    SlotScanMaster_OnFrame(&a, dup->frames[1], dup->dlc[1]);
    SlotScanMaster_OnFrame(&b, dup->frames[1], dup->dlc[1]);
    SlotScanMaster_OnFrame(&b, dup->frames[0], dup->dlc[0]);
    CHECK(SlotScanMaster_Verdict(&a) == SLOT_SCAN_DUPLICATE && SlotScanMaster_Verdict(&b) == SLOT_SCAN_DUPLICATE, "two boards are a duplicate in either order");
    /* an error frame before, between or after changes nothing for a window that is already a duplicate */
    SlotScanMaster_OnBusError(&a);
    CHECK(SlotScanMaster_Verdict(&a) == SLOT_SCAN_DUPLICATE, "errors on top of a duplicate stay a duplicate");
}

static void test_a_scan_that_was_not_begun_allows_nothing(void)
{
    SlotScanMaster m;
    memset(&m, 0, sizeof m);
    const SlotScanVectorReply *good = &slot_scan_vector_replies[1];
    CHECK(SlotScanMaster_OnFrame(&m, good->data, 8) == 0, "no scan, no answer");
    SlotScanMaster_OnBusError(&m);
    CHECK(SlotScanMaster_Verdict(&m) == SLOT_SCAN_ABSENT && SlotScanMaster_WriteAllowed(SlotScanMaster_Verdict(&m)) == 0, "never OK");
    for (unsigned slot = 8; slot < 255; slot += 17) {
        SlotScanMaster_Begin(&m, (uint8_t)slot, 7);
        CHECK(m.active == 0, "a slot outside the eight does not start a scan");
        CHECK(SlotScanMaster_OnFrame(&m, good->data, 8) == 0 && SlotScanMaster_Verdict(&m) == SLOT_SCAN_ABSENT, "and hears nothing");
    }
}

static void test_counters_saturate_instead_of_wrapping(void)
{
    SlotScanMaster m;
    const SlotScanVectorReply *good = &slot_scan_vector_replies[1]; /* slot 3, scan 7 */
    SlotScanMaster_Begin(&m, good->board_id, good->scan_id);
    for (unsigned i = 0; i < 1000; i++) SlotScanMaster_OnFrame(&m, good->data, 8);
    CHECK(m.replies == 255 && SlotScanMaster_Verdict(&m) == SLOT_SCAN_OK, "the same board repeated a thousand times is still one board");
    for (unsigned i = 0; i < 70000; i++) SlotScanMaster_OnBusError(&m);
    CHECK(m.bus_errors == 0xFFFF, "the error count stops at its maximum");
    CHECK(SlotScanMaster_Verdict(&m) == SLOT_SCAN_DUPLICATE, "errors next to one answer are a duplicate, however many");
}

/* An independent model of the rules, written from docs/SLOT_SCAN.md and not from the header. */
static SlotScanVerdict model(uint8_t slot, uint8_t scan_id, const uint32_t *uids, const uint8_t *boards, unsigned n, unsigned errors)
{
    (void)scan_id;
    unsigned distinct = 0;
    uint32_t seen[64];
    for (unsigned i = 0; i < n; i++) {
        unsigned known = 0;
        for (unsigned j = 0; j < distinct; j++) if (seen[j] == uids[i]) known = 1;
        if (!known) seen[distinct++] = uids[i];
    }
    if (distinct > 1) return SLOT_SCAN_DUPLICATE;
    if (n == 0) return errors ? SLOT_SCAN_NOISY : SLOT_SCAN_ABSENT;
    if (boards[0] != slot) return SLOT_SCAN_MISPLACED;
    return errors ? SLOT_SCAN_DUPLICATE : SLOT_SCAN_OK;
}

static uint32_t rng_state = 20240607u;
static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static void make_reply(uint8_t frame[8], uint8_t board, uint8_t role, uint8_t scan, uint32_t uid)
{
    frame[0] = SLOT_SCAN_REPLY_MAGIC;
    frame[1] = (uint8_t)(board | (role << 4));
    frame[2] = scan;
    frame[3] = (uint8_t)uid; frame[4] = (uint8_t)(uid >> 8); frame[5] = (uint8_t)(uid >> 16); frame[6] = (uint8_t)(uid >> 24);
    frame[7] = (uint8_t)(frame[0] ^ frame[1] ^ frame[2] ^ frame[3] ^ frame[4] ^ frame[5] ^ frame[6]);
}

static void test_seeded_fuzz_against_the_model(void)
{
    for (unsigned round = 0; round < 20000; round++) {
        uint8_t slot = (uint8_t)(rng() % 8u), scan_id = (uint8_t)(rng() % 4u);
        SlotScanMaster m;
        SlotScanMaster_Begin(&m, slot, scan_id);
        uint32_t uids[16];
        uint8_t boards[16];
        unsigned n = 0, errors = 0, events = rng() % 8u;
        for (unsigned e = 0; e < events; e++) {
            unsigned kind = rng() % 5u;
            if (kind == 0) { SlotScanMaster_OnBusError(&m); errors++; continue; }
            uint8_t frame[8];
            uint8_t board = (uint8_t)(rng() % 3u == 0 ? rng() % 8u : slot);
            uint32_t uid = (rng() % 2u) ? 0x1000u + (rng() % 3u) : rng();
            uint8_t this_scan = (uint8_t)(rng() % 5u == 0 ? scan_id + 1u : scan_id);
            make_reply(frame, board, (uint8_t)(rng() % 2u), this_scan, uid);
            if (kind == 1) frame[7] ^= (uint8_t)(1u + rng() % 255u); /* damaged */
            uint8_t counted = SlotScanMaster_OnFrame(&m, frame, 8);
            int should = kind != 1 && this_scan == scan_id;
            CHECK(counted == (should ? 1u : 0u), "a frame counts only when it decodes and is of this scan");
            if (should && n < 16) { uids[n] = uid; boards[n] = board; n++; }
        }
        SlotScanVerdict want = model(slot, scan_id, uids, boards, n, errors);
        SlotScanVerdict got = SlotScanMaster_Verdict(&m);
        if (got != want) { printf("  round %u: slot %u expected %d got %d\n", round, slot, (int)want, (int)got); failures++; break; }
        CHECK(SlotScanMaster_WriteAllowed(got) == (want == SLOT_SCAN_OK ? 1u : 0u), "a write is allowed only on OK");
    }
}

int main(void)
{
    test_constants_equal_the_board_side();
    test_probe_equals_the_vectors();
    test_replies_equal_the_vectors();
    test_malformed_frames_are_refused();
    test_scenarios_of_the_reference_give_the_same_verdicts();
    test_the_order_of_the_frames_does_not_change_the_verdict();
    test_a_scan_that_was_not_begun_allows_nothing();
    test_counters_saturate_instead_of_wrapping();
    test_seeded_fuzz_against_the_model();
    printf("slot_scan_master: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
