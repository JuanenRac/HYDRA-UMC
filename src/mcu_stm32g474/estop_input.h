/*
 * =============================================================================
 * estop_input.h - the physical emergency stop input and the stop category of each axis (HAL-free, host-testable)
 * PROJECT: HYDRA-UMC
 * AUTHOR: JuanenRac (Electro Hobby 3D) - electrohobby3d@gmail.com
 * LICENSE: GPL-3.0-or-later - see repo root LICENSE
 *
 * The input is a normally closed loop: it reads the healthy level while the loop is closed, and the opposite level when a contact opens, a wire is
 * cut or a pull resistor fails, so every fault is a stop. It starts TRIPPED and becomes healthy only after the healthy level has stayed for the clear
 * debounce. A stop is accepted faster than the return to healthy. The tripped input joins the stop of docs/ESTOP_CAN.md: whichever fires first latches the
 * board, and while the loop is open a RELEASE from the bus cannot unlatch it. Nothing here touches a pin; the values come from machine_config.h
 * (config/machine.cfg). Not a certified safety function.
 * =============================================================================
 */
#ifndef ESTOP_INPUT_H
#define ESTOP_INPUT_H

#include <stdint.h>

typedef struct {
    uint8_t enabled;            /* 0 = the input is ignored (a board without a stop loop) */
    uint8_t healthy_level;      /* level read while the loop is closed: 0 or 1 */
    uint32_t trip_debounce_ms;  /* the open level must stay this long to trip */
    uint32_t clear_debounce_ms; /* the healthy level must stay this long to clear */
} EstopInputConfig_t;

typedef struct {
    EstopInputConfig_t cfg;
    uint8_t tripped;        /* debounced state: 1 = loop open (or fault) */
    uint8_t candidate;      /* level being timed */
    uint8_t have_candidate;
    uint32_t candidate_since_ms;
} EstopInput_t;

static inline void EstopInput_Init(EstopInput_t *in, const EstopInputConfig_t *cfg)
{
    in->cfg = *cfg;
    in->tripped = cfg->enabled ? 1u : 0u; /* fail safe: tripped until the healthy level has been seen long enough */
    in->candidate = 0u;
    in->have_candidate = 0u;
    in->candidate_since_ms = 0u;
}

/* Call every pass with the raw level of the pin (0 or 1) and the time in ms (32 bits, may wrap). Returns the debounced tripped state. */
static inline uint8_t EstopInput_Sample(EstopInput_t *in, uint32_t now_ms, uint8_t raw_level)
{
    if (!in->cfg.enabled) return 0u;
    const uint8_t want_tripped = (uint8_t)((raw_level & 1u) != (in->cfg.healthy_level & 1u));
    if (want_tripped == in->tripped) { /* nothing to change: forget a bounce that went the other way */
        in->have_candidate = 0u;
        return in->tripped;
    }
    if (!in->have_candidate || in->candidate != want_tripped) {
        in->candidate = want_tripped;
        in->have_candidate = 1u;
        in->candidate_since_ms = now_ms;
    }
    const uint32_t needed = want_tripped ? in->cfg.trip_debounce_ms : in->cfg.clear_debounce_ms;
    if ((uint32_t)(now_ms - in->candidate_since_ms) >= needed) {
        in->tripped = want_tripped;
        in->have_candidate = 0u;
    }
    return in->tripped;
}

/* Pull resistor for the pin: opposite to the healthy level, so a cut wire or a lost contact reads as the stop level. 1 = pull up, 0 = pull down. */
static inline uint8_t EstopInput_PullUp(const EstopInputConfig_t *cfg)
{
    return (uint8_t)((cfg->healthy_level & 1u) == 0u);
}

/* ---- stop category of each axis ---- */

#define STOP_AXES 6u

typedef struct {
    uint8_t category1_mask;   /* bit n-1 set: axis n decelerates first (category 1); clear: cut at once (category 0) */
    uint32_t ramp_max_ms;
    uint8_t active;
    uint8_t cut_mask;         /* axes whose driver enable has been cut in this stop */
    uint32_t started_ms;
} StopPolicy_t;

static inline void StopPolicy_Init(StopPolicy_t *p, uint8_t category1_mask, uint32_t ramp_max_ms)
{
    p->category1_mask = (uint8_t)(category1_mask & ((1u << STOP_AXES) - 1u));
    p->ramp_max_ms = ramp_max_ms;
    p->active = 0u;
    p->cut_mask = 0u;
    p->started_ms = 0u;
}

/* A stop begins (or is renewed). Returns the axes to cut right now: every category 0 axis. The category 1 axes are listed by StopPolicy_DecelMask
 * for the motion layer and are cut by StopPolicy_Tick at the latest after ramp_max_ms. */
static inline uint8_t StopPolicy_Trip(StopPolicy_t *p, uint32_t now_ms)
{
    const uint8_t all = (uint8_t)((1u << STOP_AXES) - 1u);
    if (!p->active) {
        p->active = 1u;
        p->started_ms = now_ms;
        p->cut_mask = 0u;
    }
    const uint8_t now_cut = (uint8_t)(all & ~p->category1_mask);
    const uint8_t fresh = (uint8_t)(now_cut & ~p->cut_mask);
    p->cut_mask = (uint8_t)(p->cut_mask | now_cut);
    return fresh;
}

/* Every pass while a stop lasts: the category 1 axes whose time has run out. */
static inline uint8_t StopPolicy_Tick(StopPolicy_t *p, uint32_t now_ms)
{
    if (!p->active) return 0u;
    if ((uint32_t)(now_ms - p->started_ms) < p->ramp_max_ms) return 0u;
    const uint8_t fresh = (uint8_t)(p->category1_mask & ~p->cut_mask);
    p->cut_mask = (uint8_t)(p->cut_mask | fresh);
    return fresh;
}

/* The axes that should decelerate now (category 1 axes not yet cut), 0 when no stop is active. */
static inline uint8_t StopPolicy_DecelMask(const StopPolicy_t *p)
{
    return p->active ? (uint8_t)(p->category1_mask & ~p->cut_mask) : 0u;
}

/* The stop is over (the node is READY again): forget the cuts. */
static inline void StopPolicy_Clear(StopPolicy_t *p)
{
    p->active = 0u;
    p->cut_mask = 0u;
}

#endif /* ESTOP_INPUT_H */
