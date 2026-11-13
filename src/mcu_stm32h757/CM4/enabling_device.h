/* =============================================================================
 * HYDRA-UMC Firmware - src/mcu_stm32h757/CM4/enabling_device.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * enabling_device.h - the logic of a three-position enabling device with two redundant channels (HAL-free, host-testable). The twin of tools/enabling_device.py (read its header):
 * motion is allowed only while both channels read the middle position and the device got there from the released position; position 3 (panic), a disagreement longer than the
 * discrepancy time and a code that is not a position all ask for the stop, and the way back from panic through the middle never enables. The caller reads the two channels,
 * calls EnablingDevice_Update and sends the stop (a TRIP) when it answers that the state left ENABLED. The times are the defaults below, which tests/test_enabling_device.py holds
 * equal to config/enabling.cfg. tests/test_enabling_device.c runs it against the vectors that tools/gen_enabling_vectors.py writes from the Python reference.
 * Nothing here has run on a pendant or a board.
 * ============================================================================= */
#ifndef ENABLING_DEVICE_H
#define ENABLING_DEVICE_H

#include <stdint.h>

#ifndef ENABLING_DEBOUNCE_MS
#define ENABLING_DEBOUNCE_MS 20u
#endif
#ifndef ENABLING_DISCREPANCY_MS
#define ENABLING_DISCREPANCY_MS 100u
#endif

#define ENABLING_RELEASED 1u
#define ENABLING_MIDDLE 2u
#define ENABLING_PANIC 3u

#define ENABLING_ST_STOPPED 0u
#define ENABLING_ST_ARMED 1u
#define ENABLING_ST_ENABLED 2u
#define ENABLING_ST_LOCKED_OUT 3u
#define ENABLING_ST_FAULT 4u

typedef struct {
    uint8_t state;
    uint8_t has_released;
    uint8_t has_middle;
    uint8_t has_disagree;
    uint32_t released_since;
    uint32_t middle_since;
    uint32_t disagree_since;
    uint32_t stops;
} EnablingDevice_t;

static inline void EnablingDevice_Init(EnablingDevice_t *e)
{
    e->state = ENABLING_ST_STOPPED;
    e->has_released = 0;
    e->has_middle = 0;
    e->has_disagree = 0;
    e->released_since = 0;
    e->middle_since = 0;
    e->disagree_since = 0;
    e->stops = 0;
}

static inline uint8_t Enabling_IsPosition(uint8_t code)
{
    return (uint8_t)(code == ENABLING_RELEASED || code == ENABLING_MIDDLE || code == ENABLING_PANIC);
}

/* Returns 1 when motion is allowed after this update. e->stops counts the times the state left ENABLED: the caller asks for a stop each time it grows. */
static inline uint8_t EnablingDevice_Update(EnablingDevice_t *e, uint32_t now_ms, uint8_t channel_a, uint8_t channel_b)
{
    const uint8_t was_enabled = (uint8_t)(e->state == ENABLING_ST_ENABLED);
    if (!Enabling_IsPosition(channel_a) || !Enabling_IsPosition(channel_b)) {
        e->state = ENABLING_ST_FAULT;
        e->has_released = 0;
        e->has_middle = 0;
        e->has_disagree = 0;
    } else if (channel_a != channel_b) {
        if (!e->has_disagree) {
            e->has_disagree = 1;
            e->disagree_since = now_ms;
        }
        e->has_released = 0;
        e->has_middle = 0;
        if ((uint32_t)(now_ms - e->disagree_since) > ENABLING_DISCREPANCY_MS) {
            e->state = ENABLING_ST_FAULT;
        } else if (e->state == ENABLING_ST_ENABLED) {
            e->state = ENABLING_ST_STOPPED;
        }
    } else {
        e->has_disagree = 0;
        if (channel_a == ENABLING_PANIC) {
            e->state = ENABLING_ST_LOCKED_OUT;
            e->has_released = 0;
            e->has_middle = 0;
        } else if (channel_a == ENABLING_RELEASED) {
            e->has_middle = 0;
            if (e->state == ENABLING_ST_ENABLED) {
                e->state = ENABLING_ST_STOPPED;
            }
            if (!e->has_released) {
                e->has_released = 1;
                e->released_since = now_ms;
            }
            if ((e->state == ENABLING_ST_STOPPED || e->state == ENABLING_ST_LOCKED_OUT || e->state == ENABLING_ST_FAULT) &&
                (uint32_t)(now_ms - e->released_since) >= ENABLING_DEBOUNCE_MS) {
                e->state = ENABLING_ST_ARMED;
            }
        } else {
            e->has_released = 0;
            if (!e->has_middle) {
                e->has_middle = 1;
                e->middle_since = now_ms;
            }
            if (e->state == ENABLING_ST_ARMED && (uint32_t)(now_ms - e->middle_since) >= ENABLING_DEBOUNCE_MS) {
                e->state = ENABLING_ST_ENABLED;
            }
        }
    }
    if (was_enabled && e->state != ENABLING_ST_ENABLED) {
        e->stops++;
    }
    return (uint8_t)(e->state == ENABLING_ST_ENABLED);
}

#endif /* ENABLING_DEVICE_H */
