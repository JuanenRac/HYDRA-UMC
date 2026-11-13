/* =============================================================================
 * HYDRA-UMC Firmware - tests/test_enabling_device.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Host test of the three-position enabling device (src/mcu_stm32h757/CM4/enabling_device.h) against the vectors written from the Python reference (tests/enabling_device_vectors.h): the
 * state, the answer and the number of stops asked for must be the same at every step. Evidence: simulator.
 *
 *   cc -std=c11 -Wall -Wextra -Itests -Isrc/mcu_stm32h757/CM4 -o test_enabling_device tests/test_enabling_device.c
 */
#include <stdio.h>

#include "enabling_device.h"
#include "enabling_device_vectors.h"

int main(void)
{
    int failures = 0;
    if (ENABLING_VECTOR_DEBOUNCE_MS != ENABLING_DEBOUNCE_MS || ENABLING_VECTOR_DISCREPANCY_MS != ENABLING_DISCREPANCY_MS) {
        printf("  FAIL: the defaults of the header are not the times of config/enabling.cfg\n");
        failures++;
    }
    for (unsigned s = 0; s < ENABLING_SCENARIO_COUNT; s++) {
        const enabling_scenario_t *sc = &enabling_scenarios[s];
        EnablingDevice_t e;
        EnablingDevice_Init(&e);
        for (unsigned i = 0; i < sc->count; i++) {
            const enabling_step_t *step = &sc->steps[i];
            uint8_t enabled = EnablingDevice_Update(&e, step->t, step->a, step->b);
            if (e.state != step->state || enabled != step->enabled || e.stops != step->stops) {
                printf("  FAIL: %s step %u: state %u enabled %u stops %u, expected %u %u %u\n", sc->name, i, (unsigned)e.state, (unsigned)enabled, (unsigned)e.stops,
                       (unsigned)step->state, (unsigned)step->enabled, (unsigned)step->stops);
                failures++;
                break;
            }
        }
    }
    printf("  %u scenarios\n", (unsigned)ENABLING_SCENARIO_COUNT);
    if (failures == 0) {
        printf("enabling_device: PASS\n");
        return 0;
    }
    printf("enabling_device: FAIL (%d)\n", failures);
    return 1;
}
