/* =============================================================================
 * HYDRA-UMC Firmware - Host logic tests for the identifiers the Robot Controller
 * Board answers on and for the fragments of an answer
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * One emitter per identifier: the requests of the kinematic brain (+0x10, +0x13)
 * are never answered on their own identifier.
 */
#include <stdio.h>

#include "relay_ids.h"

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
    printf("HYDRA-UMC host logic tests - relay_ids.h\n");

    TEST_ASSERT(RelayAnswerOffset(0x10) == 0x16, "axis status is answered on +0x16");
    TEST_ASSERT(RelayAnswerOffset(0x13) == 0x18, "relay receive is answered on +0x18");
    TEST_ASSERT(RelayAnswerOffset(0x12) == 0, "relay send has no answer");
    TEST_ASSERT(RelayAnswerOffset(0x16) == 0 && RelayAnswerOffset(0x18) == 0, "an answer is not answered");
    TEST_ASSERT(RelayAnswerOffset(0x03) == 0 && RelayAnswerOffset(0x05) == 0, "the identifiers of the bootloader are not requests");
    for (uint32_t offset = 0; offset < 0x20; offset++) {
        uint8_t answer = RelayAnswerOffset(offset);
        TEST_ASSERT(answer == 0 || answer != offset, "no request is answered on its own identifier");
    }

    RelayCapturedFrame_t frame = {0x07F3, 8, {1, 2, 3, 4, 5, 6, 7, 8}};
    RelayAnswerFragments_t a;
    RelayBuildAnswerFragments(&frame, &a);
    TEST_ASSERT(a.count == 2, "8 bytes need two fragments");
    TEST_ASSERT(a.dlc[0] == 8 && a.dlc[1] == 6, "fragment lengths 3+5 and 3+3");
    TEST_ASSERT(a.data[0][0] == 0x07 && a.data[0][1] == 0xF3 && a.data[0][2] == 8, "identifier big endian and the total length");
    TEST_ASSERT(a.data[1][0] == 0x07 && a.data[1][1] == 0xF3 && a.data[1][2] == 8, "the second fragment repeats the header");
    TEST_ASSERT(a.data[0][3] == 1 && a.data[0][7] == 5 && a.data[1][3] == 6 && a.data[1][5] == 8, "bytes in order");

    RelayCapturedFrame_t five = {0x100, 5, {9, 8, 7, 6, 5}};
    RelayBuildAnswerFragments(&five, &a);
    TEST_ASSERT(a.count == 1 && a.dlc[0] == 8 && a.data[0][7] == 5, "5 bytes fit one fragment");

    RelayCapturedFrame_t empty = {0x100, 0, {0}};
    RelayBuildAnswerFragments(&empty, &a);
    TEST_ASSERT(a.count == 1 && a.dlc[0] == 3 && a.data[0][2] == 0, "an empty frame is a header only");

    RelayCapturedFrame_t big = {0x100, 200, {0}};
    RelayBuildAnswerFragments(&big, &a);
    TEST_ASSERT(a.data[0][2] == 8, "a length above 8 is cut to 8");

    uint8_t counters[2];
    RelayErrorCounters(0x00000000u, counters);
    TEST_ASSERT(counters[0] == 0 && counters[1] == 0, "a quiet bus reports zero");
    RelayErrorCounters(0x00001F0Cu, counters);
    TEST_ASSERT(counters[0] == 0x0C && counters[1] == 0x1F, "transmit counter in bits 7:0, receive counter in bits 14:8");
    RelayErrorCounters(0xFFFFFFFFu, counters);
    TEST_ASSERT(counters[0] == 0xFF && counters[1] == 0x7F, "bit 15 (receive error passive) and above are not part of the counter");

    if (failures) {
        printf("%d FAILED\n", failures);
        return 1;
    }
    printf("ALL PASSED\n");
    return 0;
}
