/* =============================================================================
 * HYDRA-UMC Firmware - src/ui_h757/core/ui_core.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * The twin of tools/ui_model.py (read its header first). Keep the two in the same order of steps: tests/test_ui_core.py compares their traces on every scenario.
 */
#include "ui_core.h"

static const uint8_t k_alarm_level[UI_AL_COUNT] = {1, 1, 1, 3, 3, 3, 1, 2, 2, 3};

static void push(ui_state_t *s, ui_effect_kind_t kind, uint8_t a, uint8_t b)
{
    if (s->fx_count < UI_MAX_EFFECTS) {
        s->fx[s->fx_count].kind = (uint8_t)kind;
        s->fx[s->fx_count].a = a;
        s->fx[s->fx_count].b = b;
        s->fx_count++;
    }
}

int ui_locked(const ui_state_t *s)
{
    return s->cfg.operator_pin_enabled != 0u && !s->pin_unlocked;
}

int ui_jog_armed(const ui_state_t *s)
{
    return s->jog_armed_until != 0u && s->now < s->jog_armed_until;
}

int ui_worst_level(const ui_state_t *s)
{
    int worst = 0;
    for (unsigned n = 0; n < UI_AL_COUNT; n++) {
        if ((s->alarms & (1u << n)) != 0u && k_alarm_level[n] > worst) {
            worst = k_alarm_level[n];
        }
    }
    return worst;
}

static uint8_t compute_link(const ui_state_t *s)
{
    int fault = s->spi_error_run >= s->cfg.spi_fault_after_errors;
    if (s->seen_heartbeat && (s->now - s->last_heartbeat) <= s->cfg.jetson_timeout_ms) {
        return UI_LINK_OK;
    }
    if (!s->seen_heartbeat && s->now < s->cfg.jetson_boot_grace_ms && !fault) {
        return UI_LINK_STARTING;
    }
    return fault ? UI_LINK_SPI_FAULT : UI_LINK_SILENT;
}

static uint8_t compute_bus(const ui_state_t *s)
{
    uint8_t worst = s->can_tx > s->can_rx ? s->can_tx : s->can_rx;
    if (s->can_off) {
        return UI_BUS_OFF;
    }
    return worst >= s->cfg.can_warn_errors ? UI_BUS_WARN : UI_BUS_OK;
}

static uint16_t compute_alarms(const ui_state_t *s)
{
    uint16_t mask = 0;
    if (s->link == UI_LINK_SILENT) {
        mask |= 1u << UI_AL_JETSON_LOST;
    }
    if (s->link == UI_LINK_SPI_FAULT) {
        mask |= 1u << UI_AL_SPI_LINK;
    }
    if (s->bus == UI_BUS_WARN) {
        mask |= 1u << UI_AL_CAN_ERRORS;
    }
    if (s->bus == UI_BUS_OFF) {
        mask |= 1u << UI_AL_CAN_BUS_OFF;
    }
    if (s->stop_pending && (s->now - s->stop_sent) >= s->cfg.stop_confirm_ms) {
        mask |= 1u << UI_AL_STOP_UNCONFIRMED;
    }
    if (s->latched && s->moving && (s->now - s->latched_since) >= s->cfg.stop_motion_ms) {
        mask |= 1u << UI_AL_MOTION_AFTER_STOP;
    }
    if (s->link == UI_LINK_OK) {
        switch (s->zone_level) {
        case 1: mask |= 1u << UI_AL_ZONE_WARNING; break;
        case 2: mask |= 1u << UI_AL_ZONE_DANGER; break;
        case 3: mask |= 1u << UI_AL_ZONE_INHIBITED; break;
        case 4: mask |= 1u << UI_AL_ZONE_FAULT; break;
        default: break;
        }
    }
    return mask;
}

static uint8_t compute_mode(const ui_state_t *s)
{
    if (s->latched || s->stop_pending) {
        return UI_MODE_STOPPED;
    }
    if (ui_worst_level(s) >= 2) {
        return UI_MODE_ALARM;
    }
    if (s->link == UI_LINK_SILENT || s->link == UI_LINK_SPI_FAULT) {
        return UI_MODE_DEGRADED;
    }
    if (s->link == UI_LINK_STARTING) {
        return UI_MODE_BOOT;
    }
    return UI_MODE_NORMAL;
}

static uint8_t compute_banner(const ui_state_t *s)
{
    if ((s->alarms & (1u << UI_AL_STOP_UNCONFIRMED)) != 0u) {
        return UI_BANNER_STOP_UNCONFIRMED;
    }
    if (s->mode == UI_MODE_STOPPED) {
        return UI_BANNER_STOPPED;
    }
    if (s->bus == UI_BUS_OFF) {
        return UI_BANNER_BUS_OFF;
    }
    if (s->mode == UI_MODE_ALARM) {
        return UI_BANNER_ALARM;
    }
    if (s->link == UI_LINK_SILENT) {
        return UI_BANNER_DEGRADED;
    }
    if (s->link == UI_LINK_SPI_FAULT) {
        return UI_BANNER_SPI_FAULT;
    }
    if (s->link == UI_LINK_STARTING) {
        return UI_BANNER_BOOT;
    }
    if (ui_locked(s)) {
        return UI_BANNER_LOCKED;
    }
    return s->listen_only ? UI_BANNER_LISTEN_ONLY : UI_BANNER_NORMAL;
}

/* Why nothing may move now, or -1. */
static int motion_refusal(const ui_state_t *s)
{
    if (s->listen_only) {
        return UI_REFUSED_LISTEN_ONLY;
    }
    if (ui_locked(s)) {
        return UI_REFUSED_LOCKED;
    }
    if (s->mode == UI_MODE_STOPPED) {
        return UI_REFUSED_STOPPED;
    }
    if (s->mode == UI_MODE_BOOT) {
        return UI_REFUSED_STARTING;
    }
    if (s->bus == UI_BUS_OFF) {
        return UI_REFUSED_BUS_FAULT;
    }
    if ((s->link == UI_LINK_SILENT || s->link == UI_LINK_SPI_FAULT) && !s->cfg.degraded_allow_motion) {
        return UI_REFUSED_DEGRADED;
    }
    if (s->mode == UI_MODE_ALARM) {
        return UI_REFUSED_ALARM;
    }
    return -1;
}

int ui_refusal(const ui_state_t *s, int action)
{
    if (action == UI_ACT_RELEASE) {
        uint16_t blocking = (uint16_t)((1u << UI_AL_ZONE_DANGER) | (1u << UI_AL_ZONE_INHIBITED) | (1u << UI_AL_ZONE_FAULT));
        if (s->listen_only) {
            return UI_REFUSED_LISTEN_ONLY;
        }
        if (ui_locked(s)) {
            return UI_REFUSED_LOCKED;
        }
        if (s->bus == UI_BUS_OFF) {
            return UI_REFUSED_BUS_FAULT;
        }
        if (!s->latched) {
            return UI_REFUSED_NOT_STOPPED;
        }
        if (s->link != UI_LINK_OK && !s->cfg.degraded_allow_release) {
            return s->link == UI_LINK_STARTING ? UI_REFUSED_STARTING : UI_REFUSED_DEGRADED;
        }
        if ((s->alarms & blocking) != 0u) {
            return UI_REFUSED_ALARM;
        }
        return -1;
    }
    if (action == UI_ACT_JOG_ARM) {
        return motion_refusal(s);
    }
    if (action > UI_ACT_JOG_ARM && action <= UI_ACT_JOG_Z_MINUS) {
        int blocked = motion_refusal(s);
        if (blocked >= 0) {
            return blocked;
        }
        return ui_jog_armed(s) ? -1 : UI_REFUSED_NOT_ARMED;
    }
    return -1;
}

static void derive(ui_state_t *s)
{
    uint16_t before = s->alarms;
    if (s->cfg.operator_pin_enabled != 0u && s->pin_unlocked && (s->now - s->last_touch) >= s->cfg.pin_idle_ms) {
        s->pin_unlocked = 0;
        push(s, UI_FX_LOCKED, 0, 0);
    }
    s->link = compute_link(s);
    s->bus = compute_bus(s);
    s->alarms = compute_alarms(s);
    s->mode = compute_mode(s);
    if (s->jog_armed_until != 0u && (!ui_jog_armed(s) || motion_refusal(s) >= 0)) {
        s->jog_armed_until = 0;
    }
    s->acked = (uint16_t)(s->acked & s->alarms);
    s->banner = compute_banner(s);
    s->caps = 0;
    for (int action = 0; action < UI_ACT_COUNT; action++) {
        if (ui_refusal(s, action) < 0) {
            s->caps = (uint16_t)(s->caps | (1u << action));
        }
    }
    for (unsigned n = 0; n < UI_AL_COUNT; n++) {
        uint16_t bit = (uint16_t)(1u << n);
        if ((s->alarms & bit) != 0u && (before & bit) == 0u) {
            push(s, UI_FX_ALARM_ON, (uint8_t)n, 0);
        } else if ((before & bit) != 0u && (s->alarms & bit) == 0u) {
            push(s, UI_FX_ALARM_OFF, (uint8_t)n, 0);
        }
    }
    if (s->hold_action >= 0 && s->hold_action != UI_ACT_TOGGLE_LISTEN_ONLY) {
        int reason = ui_refusal(s, s->hold_action);
        if (reason >= 0) {
            push(s, UI_FX_REFUSED, (uint8_t)s->hold_action, (uint8_t)reason);
            s->hold_action = UI_NO_ACTION;
        }
    }
}

static void hold_tick(ui_state_t *s)
{
    uint32_t needed;
    int action;
    if (s->hold_action < 0) {
        return;
    }
    needed = s->hold_action == UI_ACT_RELEASE ? s->cfg.release_hold_ms : s->cfg.listen_off_hold_ms;
    if ((s->now - s->hold_start) < needed) {
        return;
    }
    action = s->hold_action;
    s->hold_action = UI_NO_ACTION;
    if (action == UI_ACT_RELEASE) {
        int reason = ui_refusal(s, UI_ACT_RELEASE);
        if (reason < 0) {
            push(s, UI_FX_RELEASE, 0, 0);
        } else {
            push(s, UI_FX_REFUSED, UI_ACT_RELEASE, (uint8_t)reason);
        }
    } else {
        s->listen_only = 0;
        push(s, UI_FX_LISTEN, 0, 0);
        derive(s);
    }
}

static void touch_down(ui_state_t *s, int action)
{
    int reason;
    if (action < 0 || action >= UI_ACT_COUNT) {
        return;
    }
    if (action == UI_ACT_STOP) {
        push(s, UI_FX_STOP, 0, 0);
        s->jog_armed_until = 0;
        if (!s->latched && !s->stop_pending) {
            s->stop_pending = 1;
            s->stop_sent = s->now;
        }
        return;
    }
    if (action == UI_ACT_ACK_ALARM) {
        s->acked = s->alarms;
        push(s, UI_FX_ACK, 0, 0);
        return;
    }
    if (action == UI_ACT_NEXT_LANGUAGE) {
        s->language = (uint8_t)((s->language + 1u) % UI_LANGUAGE_COUNT);
        push(s, UI_FX_LANGUAGE, s->language, 0);
        return;
    }
    if (action == UI_ACT_TOGGLE_LISTEN_ONLY) {
        if (s->listen_only && ui_locked(s)) {
            push(s, UI_FX_REFUSED, UI_ACT_TOGGLE_LISTEN_ONLY, UI_REFUSED_LOCKED);
        } else if (s->listen_only) {
            s->hold_action = UI_ACT_TOGGLE_LISTEN_ONLY;
            s->hold_start = s->now;
        } else {
            s->listen_only = 1;
            s->jog_armed_until = 0;
            s->hold_action = UI_NO_ACTION;
            push(s, UI_FX_LISTEN, 1, 0);
        }
        return;
    }
    reason = ui_refusal(s, action);
    if (reason >= 0) {
        push(s, UI_FX_REFUSED, (uint8_t)action, (uint8_t)reason);
        return;
    }
    if (action == UI_ACT_RELEASE) {
        s->hold_action = UI_ACT_RELEASE;
        s->hold_start = s->now;
    } else if (action == UI_ACT_JOG_ARM) {
        s->jog_armed_until = s->now + s->cfg.jog_arm_ms;
        push(s, UI_FX_JOG_ARMED, 0, 0);
    } else {
        push(s, UI_FX_JOG, (uint8_t)action, 0);
    }
}

void ui_init(ui_state_t *s, const ui_config_t *cfg)
{
    uint8_t *raw = (uint8_t *)s;
    for (unsigned i = 0; i < sizeof(*s); i++) {
        raw[i] = 0;
    }
    s->cfg = *cfg;
    s->zone_level = UI_ZONE_UNKNOWN;
    s->hold_action = UI_NO_ACTION;
    derive(s);
    s->fx_count = 0;
}

static uint8_t clamp_counter(int32_t value)
{
    if (value < 0) {
        return 0;
    }
    return value > 255 ? 255u : (uint8_t)value;
}

uint8_t ui_event(ui_state_t *s, uint64_t now, ui_event_kind_t kind, int32_t a, int32_t b, int32_t c)
{
    s->fx_count = 0;
    if (now > s->now) {
        s->now = now;
    }
    switch (kind) {
    case UI_EV_HEARTBEAT:
        s->seen_heartbeat = 1;
        s->last_heartbeat = s->now;
        s->zone_level = (a >= 0 && a <= (int32_t)UI_ZONE_UNKNOWN) ? (uint8_t)a : (uint8_t)UI_ZONE_UNKNOWN;
        s->spi_error_run = 0;
        break;
    case UI_EV_SPI_ERROR:
        s->spi_error_run++;
        s->spi_errors++;
        break;
    case UI_EV_SPI_OK:
        s->spi_error_run = 0;
        break;
    case UI_EV_CAN:
        s->can_off = a != 0;
        s->can_tx = clamp_counter(b);
        s->can_rx = clamp_counter(c);
        break;
    case UI_EV_ESTOP:
        if (a != 0 && !s->latched) {
            s->latched_since = s->now;
        }
        s->latched = a != 0;
        s->stop_pending = 0;
        break;
    case UI_EV_MOTION:
        s->moving = a != 0;
        break;
    case UI_EV_NAV:
        if (a >= 0 && a < (int32_t)UI_SCREEN_COUNT) {
            s->screen = (uint8_t)a;
        }
        break;
    case UI_EV_PIN_OK:
        if (s->cfg.operator_pin_enabled != 0u && s->now >= s->pin_lockout_until) {
            s->pin_failures = 0;
            s->last_touch = s->now;
            if (!s->pin_unlocked) {
                s->pin_unlocked = 1;
                push(s, UI_FX_UNLOCKED, 0, 0);
            }
        }
        break;
    case UI_EV_PIN_FAILURES:
        if (s->cfg.operator_pin_enabled != 0u) {
            if (a >= (int32_t)s->cfg.pin_max_failures) {
                s->pin_failures = 0;
                s->pin_lockout_until = s->now + s->cfg.pin_lockout_ms;
                push(s, UI_FX_PIN_LOCKOUT, 0, 0);
            } else {
                s->pin_failures = a > 0 ? (uint32_t)a : 0u;
            }
        }
        break;
    case UI_EV_PIN_BAD:
        if (s->cfg.operator_pin_enabled != 0u && s->now >= s->pin_lockout_until) {
            s->pin_failures++;
            if (s->pin_failures >= s->cfg.pin_max_failures) {
                s->pin_failures = 0;
                s->pin_lockout_until = s->now + s->cfg.pin_lockout_ms;
                push(s, UI_FX_PIN_LOCKOUT, 0, 0);
            } else {
                push(s, UI_FX_PIN_WRONG, 0, 0);
            }
        }
        break;
    case UI_EV_TOUCH_DOWN:
        derive(s); /* a touch is judged against the state of this moment, not of the last event */
        s->last_touch = s->now;
        touch_down(s, (int)a);
        break;
    case UI_EV_TOUCH_UP:
        s->last_touch = s->now;
        if (a >= 0 && a < (int32_t)UI_ACT_COUNT && s->hold_action == (int8_t)a) {
            s->hold_action = UI_NO_ACTION;
            push(s, UI_FX_HOLD_CANCEL, 0, 0);
        }
        break;
    case UI_EV_TICK:
    default:
        break;
    }
    derive(s);
    hold_tick(s);
    return s->fx_count;
}
