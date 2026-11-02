/* =============================================================================
 * HYDRA-UMC Firmware - dumps what the real relay logic of the Robot Controller
 * Board produces, as JSON, for the golden vectors the host fakes are checked
 * against (tools/gen_relay_golden.py, tests/test_relay_golden.py)
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * It includes the header the firmware itself uses (relay_ids.h), so the bytes
 * below are the ones the board computes, not a copy of its rules.
 */
#include <stdio.h>

#include "relay_ids.h"

static const uint16_t kIds[] = {0x0000, 0x0100, 0x07F0, 0x07F3, 0x07FF};
static const unsigned kDlcs[] = {0, 1, 4, 5, 6, 7, 8, 200};
static const uint32_t kEcr[] = {0x00000000u, 0x00001F0Cu, 0x000000FFu, 0x00007F00u, 0xFFFFFFFFu};

int main(void)
{
    printf("{\n  \"answers\": [");
    for (uint32_t offset = 0; offset < 0x20; offset++) {
        printf("%s%u", offset ? ", " : "", (unsigned)RelayAnswerOffset(offset));
    }
    printf("],\n  \"counters\": [\n");
    for (unsigned i = 0; i < sizeof(kEcr) / sizeof(kEcr[0]); i++) {
        uint8_t out[2];
        RelayErrorCounters(kEcr[i], out);
        printf("    {\"ecr\": %u, \"bytes\": \"%02x%02x\"}%s\n", (unsigned)kEcr[i], out[0], out[1],
               i + 1 < sizeof(kEcr) / sizeof(kEcr[0]) ? "," : "");
    }
    printf("  ],\n  \"frames\": [\n");
    int first = 1;
    for (unsigned a = 0; a < sizeof(kIds) / sizeof(kIds[0]); a++) {
        for (unsigned b = 0; b < sizeof(kDlcs) / sizeof(kDlcs[0]); b++) {
            RelayCapturedFrame_t frame;
            memset(&frame, 0, sizeof(frame));
            frame.can_id = kIds[a];
            frame.dlc = (uint8_t)kDlcs[b];
            for (unsigned i = 0; i < 8; i++) frame.data[i] = (uint8_t)(0x11u * (i + 1u) + a);
            RelayAnswerFragments_t fragments;
            RelayBuildAnswerFragments(&frame, &fragments);
            printf("%s    {\"can_id\": %u, \"dlc\": %u, \"data\": \"", first ? "" : ",\n", (unsigned)frame.can_id, (unsigned)frame.dlc);
            unsigned shown = frame.dlc > 8 ? 8 : frame.dlc;
            for (unsigned i = 0; i < shown; i++) printf("%02x", frame.data[i]);
            printf("\", \"fragments\": [");
            for (unsigned f = 0; f < fragments.count; f++) {
                printf("%s\"", f ? ", " : "");
                for (unsigned i = 0; i < fragments.dlc[f]; i++) printf("%02x", fragments.data[f][i]);
                printf("\"");
            }
            printf("]}");
            first = 0;
        }
    }
    printf("\n  ]\n}\n");
    return 0;
}
