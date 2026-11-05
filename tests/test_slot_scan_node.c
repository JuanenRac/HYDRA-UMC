/* =============================================================================
 * HYDRA-UMC Firmware - host test of slot_scan_node.h (the board side of the slot scan)
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * The frames and the delays are the ones of tests/slot_scan_vectors.json, read through tests/slot_scan_vectors.h (tools/gen_node_vectors.py).
 */
#include <stdio.h>
#include <string.h>
#include "slot_scan_node.h"
#include "slot_scan_vectors.h"

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } } while (0)

static void test_probes(void)
{
    for (unsigned i = 0; i < sizeof slot_scan_vector_probes / sizeof slot_scan_vector_probes[0]; i++) {
        const SlotScanVectorProbe *v = &slot_scan_vector_probes[i];
        uint8_t scan_id = 0xEE;
        CHECK(SlotScan_DecodeProbe(v->data, 8, &scan_id) == 1 && scan_id == v->scan_id, v->name);
        uint8_t bad[8];
        memcpy(bad, v->data, 8);
        bad[0] ^= 1;
        CHECK(SlotScan_DecodeProbe(bad, 8, &scan_id) == 0, "wrong magic");
        memcpy(bad, v->data, 8);
        bad[3] = 1;
        CHECK(SlotScan_DecodeProbe(bad, 8, &scan_id) == 0, "non-zero filler");
        memcpy(bad, v->data, 8);
        bad[6] ^= 0xFF;
        CHECK(SlotScan_DecodeProbe(bad, 8, &scan_id) == 0, "bad complement of the magic");
        memcpy(bad, v->data, 8);
        bad[7] ^= 0x01;
        CHECK(SlotScan_DecodeProbe(bad, 8, &scan_id) == 0, "bad complement of the scan id");
        CHECK(SlotScan_DecodeProbe(v->data, 7, &scan_id) == 0, "short probe");
        CHECK(SlotScan_DecodeProbe(v->data, 0, &scan_id) == 0, "empty probe");
    }
    /* what the board itself sends is never taken for a probe */
    for (unsigned i = 0; i < sizeof slot_scan_vector_replies / sizeof slot_scan_vector_replies[0]; i++) {
        uint8_t scan_id;
        CHECK(SlotScan_DecodeProbe(slot_scan_vector_replies[i].data, 8, &scan_id) == 0, slot_scan_vector_replies[i].name);
    }
    for (unsigned i = 0; i < sizeof slot_scan_vector_malformed / sizeof slot_scan_vector_malformed[0]; i++) {
        uint8_t scan_id;
        CHECK(SlotScan_DecodeProbe(slot_scan_vector_malformed[i].data, slot_scan_vector_malformed[i].dlc, &scan_id) == 0, slot_scan_vector_malformed[i].name);
    }
}

static void test_replies(void)
{
    for (unsigned i = 0; i < sizeof slot_scan_vector_replies / sizeof slot_scan_vector_replies[0]; i++) {
        const SlotScanVectorReply *v = &slot_scan_vector_replies[i];
        uint8_t made[8];
        SlotScan_BuildReply(made, v->board_id, v->role, v->scan_id, v->uid32);
        CHECK(memcmp(made, v->data, 8) == 0, v->name);
    }
}

static void test_delays(void)
{
    for (unsigned i = 0; i < sizeof slot_scan_vector_delays / sizeof slot_scan_vector_delays[0]; i++) {
        CHECK(SlotScan_ReplyDelayMs(slot_scan_vector_delays[i].uid32) == slot_scan_vector_delays[i].delay_ms, "delay of the unique identifier");
    }
    for (uint32_t uid = 0; uid < 2000u; uid++) {
        CHECK(SlotScan_ReplyDelayMs(uid) < SLOT_SCAN_REPLY_STEPS * SLOT_SCAN_REPLY_STEP_MS, "delay inside the window");
    }
}

static void test_fold(void)
{
    CHECK(SlotScan_FoldUid(0, 0, 0) == 0, "zero");
    CHECK(SlotScan_FoldUid(0x12345678u, 0x0F0F0F0Fu, 0xF0F0F0F0u) == (0x12345678u ^ 0xFFFFFFFFu), "xor of the three words");
}

static void test_node_waits_for_its_turn(void)
{
    SlotScanNode_t node;
    SlotScanNode_Reset(&node);
    uint8_t out[8];
    CHECK(SlotScanNode_Due(&node, 0, 3, SLOT_SCAN_ROLE_APPLICATION, 1u, out) == 0, "nothing scheduled");

    const SlotScanVectorProbe *probe = &slot_scan_vector_probes[1];
    const uint32_t uid = 1u; /* delay 17 ms in the vectors */
    CHECK(SlotScanNode_OnProbe(&node, 1000, probe->data, 8, uid) == 1, "probe taken");
    CHECK(SlotScanNode_Due(&node, 1000, 3, SLOT_SCAN_ROLE_APPLICATION, uid, out) == 0, "not at once");
    CHECK(SlotScanNode_Due(&node, 1016, 3, SLOT_SCAN_ROLE_APPLICATION, uid, out) == 0, "not one step early");
    CHECK(SlotScanNode_Due(&node, 1017, 3, SLOT_SCAN_ROLE_APPLICATION, uid, out) == 1, "at its time");
    uint8_t expected[8];
    SlotScan_BuildReply(expected, 3, SLOT_SCAN_ROLE_APPLICATION, probe->scan_id, uid);
    CHECK(memcmp(out, expected, 8) == 0, "the reply carries the board, the role, the scan id and the identifier");
    CHECK(SlotScanNode_Due(&node, 1100, 3, SLOT_SCAN_ROLE_APPLICATION, uid, out) == 0, "one answer per probe");

    /* two boards of one slot answer at different times */
    SlotScanNode_t other;
    SlotScanNode_Reset(&other);
    CHECK(SlotScanNode_OnProbe(&other, 1000, probe->data, 8, 2u) == 1, "second board takes the probe");
    CHECK(SlotScanNode_Due(&other, 1002, 3, SLOT_SCAN_ROLE_APPLICATION, 2u, out) == 1, "second board answers after 2 ms");

    /* a bad frame schedules nothing; a newer probe replaces an older one that was not sent */
    SlotScanNode_Reset(&node);
    uint8_t bad[8];
    memcpy(bad, probe->data, 8);
    bad[0] = 0;
    CHECK(SlotScanNode_OnProbe(&node, 0, bad, 8, uid) == 0 && node.pending == 0, "bad probe ignored");
    CHECK(SlotScanNode_OnProbe(&node, 0, slot_scan_vector_probes[0].data, 8, uid) == 1, "first probe");
    CHECK(SlotScanNode_OnProbe(&node, 5, slot_scan_vector_probes[2].data, 8, uid) == 1, "second probe");
    CHECK(SlotScanNode_Due(&node, 100, 0, SLOT_SCAN_ROLE_BOOTLOADER, uid, out) == 1 && out[2] == slot_scan_vector_probes[2].scan_id, "the newest scan id is answered");
}

static void test_time_wraps(void)
{
    SlotScanNode_t node;
    SlotScanNode_Reset(&node);
    uint8_t out[8];
    CHECK(SlotScanNode_OnProbe(&node, 0xFFFFFFF8u, slot_scan_vector_probes[0].data, 8, 1u) == 1, "probe just before the wrap");
    CHECK(SlotScanNode_Due(&node, 0xFFFFFFFFu, 0, SLOT_SCAN_ROLE_BOOTLOADER, 1u, out) == 0, "still waiting");
    CHECK(SlotScanNode_Due(&node, 0x00000010u, 0, SLOT_SCAN_ROLE_BOOTLOADER, 1u, out) == 1, "answered after the wrap");
}

int main(void)
{
    test_probes();
    test_replies();
    test_delays();
    test_fold();
    test_node_waits_for_its_turn();
    test_time_wraps();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("slot_scan_node: all checks passed\n");
    return 0;
}
