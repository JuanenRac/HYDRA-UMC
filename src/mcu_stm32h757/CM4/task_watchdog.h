/* =============================================================================
 * HYDRA-UMC Firmware - src/mcu_stm32h757/CM4/task_watchdog.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * task_watchdog.h - the supervision of liveness by task (HAL-free, host-testable). The twin of tools/task_watchdog.py (read its header): each task calls TaskWd_CheckIn when it
 * finishes a pass; the supervisor calls TaskWd_Update every period and refreshes the hardware watchdog only when it answers 1. A mandatory task that is late asks for the stop once
 * (trips grows) and latches: it never answers 1 again, so the hardware watchdog cuts the outputs. A task that is not mandatory only marks the system degraded.
 * The deadlines and the mandatory flags are given to TaskWd_Init (the caller builds them from config/task_watchdog.cfg); tests/test_task_watchdog.c runs it against the vectors
 * that tools/gen_taskwd_vectors.py writes from the Python reference. Nothing here has run on a board.
 * ============================================================================= */
#ifndef TASK_WATCHDOG_H
#define TASK_WATCHDOG_H

#include <stdint.h>

#define TASKWD_MAX_TASKS 8u

#define TASKWD_ST_RUN 0u
#define TASKWD_ST_DEGRADED 1u
#define TASKWD_ST_TRIPPED 2u

typedef struct {
    uint8_t count;
    uint8_t state;
    uint8_t mandatory[TASKWD_MAX_TASKS];
    uint16_t late_mask;
    uint32_t deadline[TASKWD_MAX_TASKS];
    uint32_t last[TASKWD_MAX_TASKS];
    uint32_t trips;
} TaskWd_t;

/* Returns 0 when the configuration is not usable (no task, too many, or none mandatory). */
static inline uint8_t TaskWd_Init(TaskWd_t *w, uint8_t count, const uint32_t *deadline_ms, const uint8_t *mandatory, uint32_t now_ms)
{
    uint8_t any_mandatory = 0;
    if (count == 0u || count > TASKWD_MAX_TASKS) return 0;
    for (uint8_t i = 0; i < count; i++) {
        w->deadline[i] = deadline_ms[i];
        w->mandatory[i] = (uint8_t)(mandatory[i] != 0u);
        w->last[i] = now_ms;
        any_mandatory = (uint8_t)(any_mandatory || w->mandatory[i]);
    }
    w->count = count;
    w->state = TASKWD_ST_RUN;
    w->late_mask = 0;
    w->trips = 0;
    return any_mandatory;
}

static inline void TaskWd_CheckIn(TaskWd_t *w, uint8_t task, uint32_t now_ms)
{
    if (task < w->count) w->last[task] = now_ms;  /* a number that is no task is ignored */
}

/* Returns 1 when the hardware watchdog may be refreshed after this update. */
static inline uint8_t TaskWd_Update(TaskWd_t *w, uint32_t now_ms)
{
    uint16_t late = 0;
    uint8_t mandatory_late = 0;
    for (uint8_t i = 0; i < w->count; i++) {
        if ((uint32_t)(now_ms - w->last[i]) > w->deadline[i]) {
            late = (uint16_t)(late | (1u << i));
            mandatory_late = (uint8_t)(mandatory_late || w->mandatory[i]);
        }
    }
    if (w->state != TASKWD_ST_TRIPPED) {
        if (mandatory_late) {
            w->state = TASKWD_ST_TRIPPED;
            w->trips++;
        } else {
            w->state = late ? TASKWD_ST_DEGRADED : TASKWD_ST_RUN;
        }
        w->late_mask = late;
    }
    return (uint8_t)(w->state != TASKWD_ST_TRIPPED);
}

#endif /* TASK_WATCHDOG_H */
