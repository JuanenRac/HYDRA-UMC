/* =============================================================================
 * HYDRA-UMC Firmware - src/mcu_stm32h757/Common/ui_tick_plan.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * ui_tick_plan.h - the tick and the pace of the interface library (HAL-free, host-testable, independent of the library). The library wants to be told how many milliseconds went by
 * (lv_tick_inc) and to be called often enough (lv_timer_handler, which answers how long it can wait). This file turns the millisecond counter of the board into the amount to feed
 * (the counter wraps every 49 days and the arithmetic is the unsigned difference, so it does not matter; a stall longer than catchup_max_ms is fed as catchup_max_ms so that the
 * animations jump instead of replaying a burst) and holds the sleep the library asks for between handler_min_ms and handler_max_ms (the library's "no timer ready" answer, all ones,
 * is the upper bound). The numbers are those of config/lvgl.cfg. tools/ui_tick_plan.py is the Python twin. OFF by default: nothing calls this file (HYDRA_UI_TICK 0).
 * Evidence: simulator; nothing here has run on a board.
 * ============================================================================= */
#ifndef HYDRA_UMC_UI_TICK_PLAN_H
#define HYDRA_UMC_UI_TICK_PLAN_H

#include <stdint.h>

#define UTP_BOUND_MAX_MS 1000UL
#define UTP_CATCHUP_MAX_MS 10000UL
#define UTP_NO_TIMER_READY 0xFFFFFFFFUL /* the answer of the library when no timer is waiting */

#define UTP_OK 0
#define UTP_E_ARG 1
#define UTP_E_CONFIG 2 /* a bound outside 1..1000 ms, a minimum above the maximum, or a catch-up outside 1..10000 ms */
#define UTP_CLAMPED 3  /* the stall was longer than the catch-up: the amount fed is the catch-up */

typedef struct {
    uint32_t catchup_max_ms;
    uint32_t handler_min_ms;
    uint32_t handler_max_ms;
} UtpConfig;

typedef struct {
    uint32_t last_ms;
} UtpState;

static inline int Utp_Check(const UtpConfig *c)
{
    if (c == 0) return UTP_E_ARG;
    if (c->handler_min_ms < 1UL || c->handler_max_ms > UTP_BOUND_MAX_MS || c->handler_min_ms > c->handler_max_ms) return UTP_E_CONFIG;
    if (c->catchup_max_ms < 1UL || c->catchup_max_ms > UTP_CATCHUP_MAX_MS) return UTP_E_CONFIG;
    return UTP_OK;
}

static inline int Utp_Init(UtpState *s, uint32_t now_ms)
{
    if (s == 0) return UTP_E_ARG;
    s->last_ms = now_ms;
    return UTP_OK;
}

/* The milliseconds to feed the library now (0 when none went by). UTP_CLAMPED when the stall was longer than the catch-up. */
static inline int Utp_Tick(UtpState *s, const UtpConfig *c, uint32_t now_ms, uint32_t *feed_ms)
{
    uint32_t elapsed;
    int rc = Utp_Check(c);
    if (rc != UTP_OK) return rc;
    if (s == 0 || feed_ms == 0) return UTP_E_ARG;
    elapsed = now_ms - s->last_ms;
    s->last_ms = now_ms;
    if (elapsed > c->catchup_max_ms) {
        *feed_ms = c->catchup_max_ms;
        return UTP_CLAMPED;
    }
    *feed_ms = elapsed;
    return UTP_OK;
}

/* How long the interface task sleeps: what the library asked for, held between the bounds. */
static inline uint32_t Utp_SleepMs(const UtpConfig *c, uint32_t asked_ms)
{
    if (asked_ms < c->handler_min_ms) return c->handler_min_ms;
    if (asked_ms > c->handler_max_ms) return c->handler_max_ms;
    return asked_ms;
}

#endif /* HYDRA_UMC_UI_TICK_PLAN_H */
