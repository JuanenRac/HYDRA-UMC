/* =============================================================================
 * HYDRA-UMC Firmware - Host logic tests for the RELAY_SEND fragment reassembly
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * A frame cut into a 5-byte and a 3-byte fragment must leave the relay as ONE
 * 8-byte frame; the old code forwarded each fragment as its own short frame.
 */
#include <stdio.h>

#include "relay_send_reassembly.h"

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("  FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
            failures++; \
        } \
    } while (0)

static int failures;

int main(void)
{
    printf("HYDRA-UMC host logic tests - relay_send_reassembly.h\n");
    RelaySendReassembly_t r;
    RelaySendReassembly_Reset(&r);
    uint16_t id = 0;
    uint8_t data[8];
    uint8_t dlc = 0;

    /* 8 bytes: 5 + 3 -> one frame of 8 */
    const uint8_t first[5] = {1, 2, 3, 4, 5};
    const uint8_t second[3] = {6, 7, 8};
    TEST_ASSERT(RelaySendReassembly_Feed(&r, 100, 0x7FE, 8, first, 5, &id, data, &dlc) == 0, "first fragment alone emits nothing");
    TEST_ASSERT(RelaySendReassembly_Feed(&r, 105, 0x7FE, 8, second, 3, &id, data, &dlc) == 1, "second fragment completes the frame");
    TEST_ASSERT(id == 0x7FE && dlc == 8, "id and dlc of the whole frame");
    TEST_ASSERT(data[0] == 1 && data[4] == 5 && data[5] == 6 && data[7] == 8, "bytes in order");

    /* 4 bytes: a single fragment goes out at once */
    const uint8_t four[4] = {9, 8, 7, 6};
    TEST_ASSERT(RelaySendReassembly_Feed(&r, 200, 0x7F0, 4, four, 4, &id, data, &dlc) == 1, "4-byte frame needs one fragment");
    TEST_ASSERT(dlc == 4 && data[3] == 6, "4-byte payload intact");

    /* 5 bytes exactly: one fragment */
    TEST_ASSERT(RelaySendReassembly_Feed(&r, 210, 0x100, 5, first, 5, &id, data, &dlc) == 1 && dlc == 5, "5-byte frame is a single fragment");

    /* 0 bytes: an empty frame is still a frame */
    TEST_ASSERT(RelaySendReassembly_Feed(&r, 220, 0x100, 0, first, 0, &id, data, &dlc) == 1 && dlc == 0, "empty frame passes");

    /* second fragment without a first one is dropped */
    TEST_ASSERT(RelaySendReassembly_Feed(&r, 300, 0x7FE, 8, second, 3, &id, data, &dlc) == 0, "orphan second fragment dropped");

    /* second fragment too late is dropped */
    RelaySendReassembly_Feed(&r, 400, 0x7FE, 8, first, 5, &id, data, &dlc);
    TEST_ASSERT(RelaySendReassembly_Feed(&r, 400 + RELAY_SEND_REASSEMBLY_TIMEOUT_MS, 0x7FE, 8, second, 3, &id, data, &dlc) == 0, "late second fragment dropped");

    /* second fragment for another target is dropped */
    RelaySendReassembly_Feed(&r, 500, 0x7FE, 8, first, 5, &id, data, &dlc);
    TEST_ASSERT(RelaySendReassembly_Feed(&r, 501, 0x7FD, 8, second, 3, &id, data, &dlc) == 0, "other target's fragment dropped");

    /* a new first fragment replaces a stale pending one */
    RelaySendReassembly_Feed(&r, 600, 0x7FE, 8, first, 5, &id, data, &dlc);
    RelaySendReassembly_Feed(&r, 601, 0x7FE, 8, first, 5, &id, data, &dlc);
    TEST_ASSERT(RelaySendReassembly_Feed(&r, 602, 0x7FE, 8, second, 3, &id, data, &dlc) == 1, "restarted frame completes");

    /* bad ids and lengths */
    TEST_ASSERT(RelaySendReassembly_Feed(&r, 700, 0x804, 4, four, 4, &id, data, &dlc) == 0, "id above 0x7FF dropped");
    TEST_ASSERT(RelaySendReassembly_Feed(&r, 701, 0x100, 9, first, 5, &id, data, &dlc) == 0, "total above 8 dropped");
    TEST_ASSERT(RelaySendReassembly_Feed(&r, 702, 0x100, 4, first, 5, &id, data, &dlc) == 0, "fragment longer than the frame dropped");
    TEST_ASSERT(RelaySendReassembly_Feed(&r, 703, 0x100, 8, first, 4, &id, data, &dlc) == 0, "second-sized fragment with no first dropped");

    /* tick wrap-around still works */
    RelaySendReassembly_Feed(&r, 0xFFFFFFF0U, 0x7FE, 8, first, 5, &id, data, &dlc);
    TEST_ASSERT(RelaySendReassembly_Feed(&r, 0xFFFFFFF5U, 0x7FE, 8, second, 3, &id, data, &dlc) == 1, "completes across tick wrap");

    if (failures == 0) {
        printf("  OK   all relay_send_reassembly host tests passed\n");
        return 0;
    }
    printf("  %d assertion(s) failed\n", failures);
    return 1;
}
