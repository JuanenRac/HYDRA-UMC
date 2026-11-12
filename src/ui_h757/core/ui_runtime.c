/* =============================================================================
 * HYDRA-UMC Firmware - src/ui_h757/core/ui_runtime.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 */
#include "ui_runtime.h"

static void dispatch(ui_runtime_t *rt)
{
    for (uint8_t i = 0; i < rt->core.fx_count; i++) {
        const ui_effect_t *fx = &rt->core.fx[i];
        switch (fx->kind) {
        case UI_FX_STOP:
            /* the view-model also reports a STOP it was told about; the stop itself was already sent by ui_runtime_touch */
            break;
        case UI_FX_RELEASE:
            if (rt->ports.send_release != 0) {
                rt->ports.send_release(rt->ports.context);
            }
            break;
        case UI_FX_JOG:
            if (rt->ports.send_jog != 0) {
                rt->ports.send_jog(rt->ports.context, fx->a);
            }
            break;
        case UI_FX_LISTEN:
            if (rt->ports.save_setting != 0) {
                rt->ports.save_setting(rt->ports.context, UI_SETTING_LISTEN_ONLY, fx->a);
            }
            break;
        case UI_FX_LANGUAGE:
            if (rt->ports.save_setting != 0) {
                rt->ports.save_setting(rt->ports.context, UI_SETTING_LANGUAGE, fx->a);
            }
            break;
        default:
            break;
        }
    }
}

static void feed(ui_runtime_t *rt, uint64_t now_ms, ui_event_kind_t kind, int32_t a, int32_t b, int32_t c)
{
    (void)ui_event(&rt->core, now_ms, kind, a, b, c);
    dispatch(rt);
}

static void wipe_pad(ui_runtime_t *rt)
{
    for (unsigned i = 0; i < sizeof(rt->pad); i++) {
        rt->pad[i] = 0;
    }
    rt->pad_length = 0;
}

void ui_runtime_init(ui_runtime_t *rt, const ui_config_t *cfg, const ui_runtime_ports_t *ports, const ui_pin_store_t *pin, uint8_t *picture, uint32_t capacity)
{
    rt->ports = *ports;
    rt->pin = *pin;
    rt->stops_sent = 0;
    rt->pad_max_digits = pin->max_digits < UI_PAD_MAX_DIGITS ? pin->max_digits : UI_PAD_MAX_DIGITS;
    wipe_pad(rt);
    ui_init(&rt->core, cfg);
    ui_link_init(&rt->link, ports->spi_transact, ports->spi_context, picture, capacity, 100, 500);
    feed(rt, 0, UI_EV_PIN_FAILURES, (int32_t)ui_pin_failures(&rt->pin), 0, 0);
}

void ui_runtime_poll(ui_runtime_t *rt, uint64_t now_ms)
{
    if (rt->ports.spi_transact != 0) {
        ui_link_event_t events[2];
        uint32_t count = ui_link_step(&rt->link, now_ms, events, 2);
        for (uint32_t i = 0; i < count; i++) {
            if (events[i].kind == UI_LINK_EV_HEARTBEAT) {
                feed(rt, now_ms, UI_EV_HEARTBEAT, events[i].zone_level, 0, 0);
            } else if (events[i].kind == UI_LINK_EV_SPI_OK) {
                feed(rt, now_ms, UI_EV_SPI_OK, 0, 0, 0);
            } else {
                feed(rt, now_ms, UI_EV_SPI_ERROR, 0, 0, 0);
            }
        }
    }
    feed(rt, now_ms, UI_EV_TICK, 0, 0, 0);
}

void ui_runtime_touch(ui_runtime_t *rt, uint64_t now_ms, int action, int down)
{
    if (down && action == UI_ACT_STOP) {
        /* FIRST: the stop. Nothing before it, nothing that waits. */
        if (rt->ports.send_stop != 0) {
            rt->ports.send_stop(rt->ports.context, UI_STOP_REASON_OPERATOR);
        }
        rt->stops_sent++;
        feed(rt, now_ms, UI_EV_TOUCH_DOWN, action, 0, 0);
        return;
    }
    feed(rt, now_ms, down ? UI_EV_TOUCH_DOWN : UI_EV_TOUCH_UP, action, 0, 0);
}

void ui_runtime_bus(ui_runtime_t *rt, uint64_t now_ms, int bus_off, int tx_errors, int rx_errors)
{
    feed(rt, now_ms, UI_EV_CAN, bus_off, tx_errors, rx_errors);
}

void ui_runtime_estop(ui_runtime_t *rt, uint64_t now_ms, int latched)
{
    feed(rt, now_ms, UI_EV_ESTOP, latched, 0, 0);
}

void ui_runtime_motion(ui_runtime_t *rt, uint64_t now_ms, int moving)
{
    feed(rt, now_ms, UI_EV_MOTION, moving, 0, 0);
}

void ui_runtime_navigate(ui_runtime_t *rt, uint64_t now_ms, int screen)
{
    feed(rt, now_ms, UI_EV_NAV, screen, 0, 0);
}

void ui_runtime_pad_digit(ui_runtime_t *rt, int digit)
{
    if (digit >= 0 && digit <= 9 && rt->pad_length < rt->pad_max_digits) {
        rt->pad[rt->pad_length++] = (uint8_t)('0' + digit);
        rt->pad[rt->pad_length] = 0;
    }
}

void ui_runtime_pad_clear(ui_runtime_t *rt)
{
    wipe_pad(rt);
}

void ui_runtime_pad_enter(ui_runtime_t *rt, uint64_t now_ms)
{
    ui_pin_result_t result;
    if (rt->pad_length < rt->pin.min_digits || now_ms < rt->core.pin_lockout_until) {
        wipe_pad(rt);
        return; /* too short to be a try, or the pad is locked out: nothing is checked and nothing is counted (the store would clear its counter on a right PIN) */
    }
    result = ui_pin_verify(&rt->pin, (const char *)rt->pad);
    wipe_pad(rt);
    feed(rt, now_ms, result == UI_PIN_OK ? UI_EV_PIN_OK : UI_EV_PIN_BAD, 0, 0, 0);
}
