/* =============================================================================
 * HYDRA-UMC Firmware - tests/test_zones_frame.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Host test of the filter of the brain for the frame of the safety-zones service on identifier 0x002 (src/mcu_stm32h757/CM4/zones_frame.h) against the vectors written from the Python
 * reference (tests/zones_frame_vectors.h). Built twice: with HYDRA_ZONES_CAN_FILTER=1 the scenarios are replayed; with the default (0) every answer must be 0 and the filter must not
 * program anything. Evidence: simulator.
 *
 *   cc -std=c11 -Wall -Wextra -DHYDRA_ZONES_CAN_FILTER=1 -Itests -Isrc/mcu_stm32h757/CM4 -o test_zones_frame tests/test_zones_frame.c
 */
#include <stdio.h>
#include <string.h>

#include "zones_frame.h"
#include "zones_frame_vectors.h"

int main(void)
{
    int failures = 0;
    for (unsigned s = 0; s < ZONES_SCENARIO_COUNT; s++) {
        const zones_scenario_t *sc = &zones_scenarios[s];
        ZonesFilter_t z;
        ZonesFilter_Init(&z);
        for (unsigned i = 0; i < sc->count; i++) {
            const zones_step_t *step = &sc->steps[i];
            uint8_t got = step->op == 'F' ? ZonesFilter_OnFrame(&z, step->t, step->id, step->data, step->dlc) : ZonesFilter_Tick(&z, step->t);
#if HYDRA_ZONES_CAN_FILTER
            if (got != step->expected) {
                printf("  FAIL: %s step %u: got %u, expected %u\n", sc->name, i, (unsigned)got, (unsigned)step->expected);
                failures++;
                break;
            }
#else
            if (got != 0u) {
                printf("  FAIL: %s step %u: the switch is off and the filter answered %u\n", sc->name, i, (unsigned)got);
                failures++;
                break;
            }
#endif
        }
    }
    {
        /* the CRC of a known frame: kind 1, version 1, sequence 0x0201, subject 0x0403, breached 5 */
        const uint8_t body[7] = {1, 1, 0x01, 0x02, 0x03, 0x04, 5};
        const uint8_t check = Zones_Crc8(body, 7);
        uint8_t frame[8];
        ZonesFilter_t z;
        memcpy(frame, body, 7);
        frame[7] = check;
        ZonesFilter_Init(&z);
        if (HYDRA_ZONES_CAN_FILTER && ZonesFilter_OnFrame(&z, 0, ZONES_CAN_ID, frame, 8) != 1u) {
            printf("  FAIL: a valid stop frame did not trip\n");
            failures++;
        }
    }
    if (HYDRA_ZONES_CAN_FILTER) {
        /* the service starts again with a lower counter after the loss: its heartbeat arms the watch again */
        const uint8_t high[7] = {2, 1, 0x40, 0x9C, 0, 0, 0};
        const uint8_t low[7] = {2, 1, 0x01, 0x00, 0, 0, 0};
        uint8_t frame[8];
        ZonesFilter_t z;
        ZonesFilter_Init(&z);
        memcpy(frame, high, 7);
        frame[7] = Zones_Crc8(high, 7);
        (void)ZonesFilter_OnFrame(&z, 0, ZONES_CAN_ID, frame, 8);
        const uint8_t first = ZonesFilter_Tick(&z, 1001);
        memcpy(frame, low, 7);
        frame[7] = Zones_Crc8(low, 7);
        (void)ZonesFilter_OnFrame(&z, 2000, ZONES_CAN_ID, frame, 8);
        if (first != 1u || ZonesFilter_Tick(&z, 2500) != 0u || ZonesFilter_Tick(&z, 3001) != 1u) {
            printf("  FAIL: the watch was not armed again by a restarted service\n");
            failures++;
        }
    }
    printf("  %u scenarios, switch %s\n", (unsigned)ZONES_SCENARIO_COUNT, HYDRA_ZONES_CAN_FILTER ? "on" : "off");
    if (failures == 0) {
        printf("zones_frame: PASS\n");
        return 0;
    }
    printf("zones_frame: FAIL (%d)\n", failures);
    return 1;
}
