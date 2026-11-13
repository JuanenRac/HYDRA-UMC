/* =============================================================================
 * HYDRA-UMC Firmware - tests/test_task_watchdog.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Host test of the supervision of liveness by task (src/mcu_stm32h757/CM4/task_watchdog.h) against the vectors written from the Python reference (tests/task_watchdog_vectors.h):
 * the state, the late mask, the trips and the answer must be the same after every update. Evidence: simulator.
 *
 *   cc -std=c11 -Wall -Wextra -Itests -Isrc/mcu_stm32h757/CM4 -o test_task_watchdog tests/test_task_watchdog.c
 */
#include <stdio.h>

#include "task_watchdog.h"
#include "task_watchdog_vectors.h"

int main(void)
{
    int failures = 0;
    for (unsigned s = 0; s < TASKWD_SCENARIO_COUNT; s++) {
        const taskwd_scenario_t *sc = &taskwd_scenarios[s];
        TaskWd_t w;
        if (!TaskWd_Init(&w, TASKWD_VECTOR_TASKS, taskwd_vector_deadline, taskwd_vector_mandatory, sc->start)) {
            printf("  FAIL: %s: the configuration was refused\n", sc->name);
            failures++;
            continue;
        }
        for (unsigned i = 0; i < sc->count; i++) {
            const taskwd_step_t *step = &sc->steps[i];
            if (!step->is_update) {
                TaskWd_CheckIn(&w, step->task, step->t);
                continue;
            }
            uint8_t refresh = TaskWd_Update(&w, step->t);
            if (w.state != step->state || w.late_mask != step->late || w.trips != step->trips || refresh != step->refresh) {
                printf("  FAIL: %s step %u: state %u late %u trips %u refresh %u, expected %u %u %u %u\n", sc->name, i, (unsigned)w.state, (unsigned)w.late_mask, (unsigned)w.trips,
                       (unsigned)refresh, (unsigned)step->state, (unsigned)step->late, (unsigned)step->trips, (unsigned)step->refresh);
                failures++;
                break;
            }
        }
    }
    /* a configuration that cannot work is refused */
    {
        TaskWd_t w;
        uint32_t d[2] = {10u, 10u};
        uint8_t none[2] = {0, 0}, one[2] = {0, 1};
        if (TaskWd_Init(&w, 0, d, one, 0) || TaskWd_Init(&w, TASKWD_MAX_TASKS + 1u, d, one, 0) || TaskWd_Init(&w, 2, d, none, 0) || !TaskWd_Init(&w, 2, d, one, 0)) {
            printf("  FAIL: the check of the configuration\n");
            failures++;
        }
    }
    printf("  %u scenarios, worst time to the cut %u ms\n", (unsigned)TASKWD_SCENARIO_COUNT, (unsigned)TASKWD_VECTOR_BOUND_MS);
    if (failures == 0) {
        printf("task_watchdog: PASS\n");
        return 0;
    }
    printf("task_watchdog: FAIL (%d)\n", failures);
    return 1;
}
