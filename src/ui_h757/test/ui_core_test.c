/* =============================================================================
 * HYDRA-UMC Firmware - src/ui_h757/test/ui_core_test.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Host test of the view-model in C (src/ui_h757/core/ui_core.c): every scenario of src/ui_h757/spec/scenarios.json is replayed and its trace is compared, line by line, with the one the
 * Python reference (tools/ui_model.py) wrote into src/ui_h757/test/ui_core_vectors.h; then a fuzz with a fixed seed checks the rules that never change. Evidence: simulator.
 *
 *   cc -std=c11 -Wall -Wextra -Isrc/ui_h757/core -Isrc/ui_h757/test -o ui_core_test src/ui_h757/test/ui_core_test.c src/ui_h757/core/ui_core.c
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ui_core.h"
#include "ui_config.h"
#include "ui_screens.h"
#include "ui_core_vectors.h"

static const char *const k_modes[] = {"boot", "normal", "degraded", "alarm", "stopped"};
static const char *const k_links[] = {"starting", "ok", "silent", "spi_fault"};
static const char *const k_buses[] = {"ok", "warn", "off"};
static const char *const k_banners[] = {"normal", "boot", "degraded", "spi_fault", "bus_off", "stopped", "listen_only", "alarm", "stop_unconfirmed"};
static const char *const k_actions[] = {"stop", "release", "jog_arm", "jog_x_plus", "jog_x_minus", "jog_y_plus", "jog_y_minus", "jog_z_plus", "jog_z_minus",
                                        "ack_alarm", "toggle_listen_only", "next_language"};
static const char *const k_reasons[] = {"listen_only", "not_stopped", "stopped", "degraded", "bus_fault", "not_armed", "alarm", "starting"};
static const char *const k_alarms[] = {"jetson_lost", "spi_link", "can_errors", "can_bus_off", "stop_unconfirmed", "motion_after_stop", "zone_warning", "zone_danger",
                                       "zone_inhibited", "zone_fault"};

static void append(char *out, size_t size, const char *format, ...) __attribute__((format(printf, 3, 4)));
static void append(char *out, size_t size, const char *format, ...)
{
    size_t used = strlen(out);
    va_list args;
    va_start(args, format);
    if (used < size) {
        vsnprintf(out + used, size - used, format, args);
    }
    va_end(args);
}

static ui_event_kind_t kind_of(const char *name)
{
    static const struct { const char *name; ui_event_kind_t kind; } table[] = {
        {"TICK", UI_EV_TICK}, {"HB", UI_EV_HEARTBEAT}, {"SPI_ERR", UI_EV_SPI_ERROR}, {"SPI_OK", UI_EV_SPI_OK}, {"CAN", UI_EV_CAN}, {"ESTOP", UI_EV_ESTOP},
        {"MOTION", UI_EV_MOTION}, {"NAV", UI_EV_NAV}, {"DOWN", UI_EV_TOUCH_DOWN}, {"UP", UI_EV_TOUCH_UP},
    };
    for (unsigned i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (strcmp(table[i].name, name) == 0) {
            return table[i].kind;
        }
    }
    return UI_EV_TICK;
}

static void format_line(const ui_state_t *s, const char *name, char *out, size_t size)
{
    unsigned unacked = (unsigned)(s->alarms & ~s->acked);
    out[0] = '\0';
    append(out, size, "t=%llu ev=%s mode=%s link=%s bus=%s lo=%d scr=%u al=%03x un=%03x arm=%d stop=%d%d hold=%s ban=%s caps=%04x fx=", (unsigned long long)s->now, name,
           k_modes[s->mode], k_links[s->link], k_buses[s->bus], s->listen_only, (unsigned)s->screen, (unsigned)s->alarms, unacked, ui_jog_armed(s), s->latched,
           s->stop_pending, s->hold_action >= 0 ? k_actions[s->hold_action] : "-", k_banners[s->banner], (unsigned)s->caps);
    if (s->fx_count == 0) {
        append(out, size, "-");
    }
    for (unsigned i = 0; i < s->fx_count; i++) {
        const ui_effect_t *fx = &s->fx[i];
        if (i > 0) {
            append(out, size, ",");
        }
        switch (fx->kind) {
        case UI_FX_STOP: append(out, size, "stop"); break;
        case UI_FX_RELEASE: append(out, size, "release"); break;
        case UI_FX_JOG: append(out, size, "jog:%s", k_actions[fx->a] + 4); break;
        case UI_FX_JOG_ARMED: append(out, size, "jog_armed"); break;
        case UI_FX_LISTEN: append(out, size, "listen:%u", (unsigned)fx->a); break;
        case UI_FX_LANGUAGE: append(out, size, "lang:%u", (unsigned)fx->a); break;
        case UI_FX_ACK: append(out, size, "ack"); break;
        case UI_FX_REFUSED: append(out, size, "refused:%s:%s", k_actions[fx->a], k_reasons[fx->b]); break;
        case UI_FX_ALARM_ON: append(out, size, "alarm+:%s", k_alarms[fx->a]); break;
        case UI_FX_ALARM_OFF: append(out, size, "alarm-:%s", k_alarms[fx->a]); break;
        case UI_FX_HOLD_CANCEL: append(out, size, "hold_cancel"); break;
        default: append(out, size, "?"); break;
        }
    }
}

static int scenarios(void)
{
    int failures = 0;
    for (unsigned n = 0; n < UI_VECTOR_SCENARIO_COUNT; n++) {
        const ui_vector_scenario_t *sc = &ui_vector_scenarios[n];
        ui_config_t cfg;
        ui_state_t s;
        memcpy(&cfg, sc->cfg, sizeof(cfg));
        ui_init(&s, &cfg);
        for (unsigned i = 0; i < sc->count; i++) {
            char line[512];
            const ui_vector_event_t *ev = &sc->events[i];
            ui_event(&s, ev->t, kind_of(ev->name), ev->a, ev->b, ev->c);
            format_line(&s, ev->name, line, sizeof(line));
            if (strcmp(line, sc->expected[i]) != 0) {
                printf("  FAIL: %s step %u\n    C:      %s\n    Python: %s\n", sc->name, i, line, sc->expected[i]);
                failures++;
                break;
            }
        }
    }
    printf("  %u scenarios replayed in C\n", UI_VECTOR_SCENARIO_COUNT);
    return failures;
}

static uint32_t g_seed = 12345u;
static uint32_t next_random(void)
{
    g_seed = g_seed * 1664525u + 1013904223u;
    return g_seed >> 8;
}

/* The rules that never change, held over a long random run (fixed seed, deterministic). */
static int fuzz(int allow_release, int allow_motion)
{
    int failures = 0;
    ui_config_t cfg = UI_CONFIG_DEFAULT;
    ui_state_t s;
    uint64_t now = 0;
    cfg.degraded_allow_release = (uint32_t)allow_release;
    cfg.degraded_allow_motion = (uint32_t)allow_motion;
    ui_init(&s, &cfg);
    for (unsigned step = 0; step < 200000u; step++) {
        uint32_t r = next_random();
        ui_event_kind_t kind = (ui_event_kind_t)(r % 10u);
        int32_t a = 0, b = 0, c = 0;
        int listen_before, latched_before;
        now += (r >> 4) % 700u;
        switch (kind) {
        case UI_EV_HEARTBEAT: a = (int32_t)((r >> 8) % 7u); break;
        case UI_EV_CAN: a = (int32_t)((r >> 8) % 4u == 0u); b = (int32_t)((r >> 10) % 200u); c = (int32_t)((r >> 12) % 200u); break;
        case UI_EV_ESTOP: a = (int32_t)((r >> 8) % 3u == 0u); break;
        case UI_EV_MOTION: a = (int32_t)((r >> 8) & 1u); break;
        case UI_EV_NAV: a = (int32_t)((r >> 8) % 10u); break;
        case UI_EV_TOUCH_DOWN:
        case UI_EV_TOUCH_UP: a = (int32_t)((r >> 8) % 14u) - 1; break;
        default: break;
        }
        if (kind == UI_EV_TOUCH_DOWN && a == UI_ACT_STOP) {
            /* STOP is never refused, in any state */
            ui_event(&s, now, kind, a, b, c);
            int sent = 0;
            for (unsigned i = 0; i < s.fx_count; i++) {
                sent |= s.fx[i].kind == UI_FX_STOP;
            }
            if (!sent || s.mode != UI_MODE_STOPPED) {
                printf("  FAIL: STOP not sent or the mode is not stopped (step %u)\n", step);
                return failures + 1;
            }
            continue;
        }
        listen_before = s.listen_only;
        latched_before = s.latched;
        ui_event(&s, now, kind, a, b, c);
        for (unsigned i = 0; i < s.fx_count; i++) {
            const ui_effect_t *fx = &s.fx[i];
            if (fx->kind == UI_FX_JOG || fx->kind == UI_FX_JOG_ARMED) {
                int degraded = s.link == UI_LINK_SILENT || s.link == UI_LINK_SPI_FAULT;
                if (s.listen_only || listen_before || s.mode == UI_MODE_STOPPED || s.mode == UI_MODE_BOOT || s.bus == UI_BUS_OFF || (degraded && !allow_motion)) {
                    printf("  FAIL: a jog effect in a state that forbids it (step %u, mode %u link %u)\n", step, (unsigned)s.mode, (unsigned)s.link);
                    return failures + 1;
                }
                if (s.mode == UI_MODE_ALARM) {
                    printf("  FAIL: a jog effect in alarm mode (step %u)\n", step);
                    return failures + 1;
                }
            }
            if (fx->kind == UI_FX_RELEASE) {
                int degraded = s.link != UI_LINK_OK;
                if (!latched_before || s.listen_only || s.bus == UI_BUS_OFF || (degraded && !allow_release)) {
                    printf("  FAIL: a release effect in a state that forbids it (step %u)\n", step);
                    return failures + 1;
                }
            }
        }
        if (s.caps & (1u << UI_ACT_STOP)) {
            /* stop allowed bit is always set */
        } else {
            printf("  FAIL: the STOP bit of caps is clear (step %u)\n", step);
            return failures + 1;
        }
        if (s.listen_only && (s.caps & ((1u << UI_ACT_RELEASE) | (1u << UI_ACT_JOG_ARM) | (1u << UI_ACT_JOG_X_PLUS))) != 0u) {
            printf("  FAIL: a command is allowed while listen-only is on (step %u)\n", step);
            return failures + 1;
        }
        if (s.latched && s.mode != UI_MODE_STOPPED) {
            printf("  FAIL: latched but not in the stopped mode (step %u)\n", step);
            return failures + 1;
        }
        if (s.link == UI_LINK_OK && s.mode == UI_MODE_DEGRADED) {
            printf("  FAIL: degraded with the Jetson answering (step %u)\n", step);
            return failures + 1;
        }
    }
    return failures;
}

static int tables(void)
{
    int failures = 0;
    /* the STOP button is a global widget on every screen, large, and bound to the first action */
    int stop_buttons = 0;
    for (unsigned i = 0; i < UI_GLOBAL_WIDGET_COUNT; i++) {
        const ui_widget_t *w = &ui_global_widgets[i];
        if (w->action == UI_ACTID_STOP) {
            stop_buttons++;
            if (w->w < 240 || w->h < 88) {
                printf("  FAIL: the STOP button is too small\n");
                failures++;
            }
        }
    }
    if (stop_buttons != 1 || (int)UI_ACTID_STOP != (int)UI_ACT_STOP || (int)UI_ACTID_NEXT_LANGUAGE != (int)UI_ACT_NEXT_LANGUAGE) {
        printf("  FAIL: the action tables do not agree with the core\n");
        failures++;
    }
    for (unsigned n = 0; n < UI_SCREEN_TABLE_COUNT; n++) {
        for (unsigned i = 0; i < ui_screens[n].count; i++) {
            const ui_widget_t *w = &ui_screens[n].widgets[i];
            if (w->x + w->w > UI_FRAME_WIDTH || w->y + w->h > UI_FRAME_HEIGHT) {
                printf("  FAIL: a widget outside the frame\n");
                failures++;
            }
            if (w->label != UI_NO_TEXT && w->label >= UI_T_COUNT) {
                printf("  FAIL: a text id out of range\n");
                failures++;
            }
        }
    }
    for (unsigned t = 0; t < UI_T_COUNT; t++) {
        for (unsigned lang = 0; lang < UI_LANG_COUNT; lang++) {
            if (ui_texts[t][lang] == 0 || ui_texts[t][lang][0] == '\0') {
                printf("  FAIL: an empty text\n");
                failures++;
            }
        }
    }
    printf("  tables: %u screens, %u texts in %u languages\n", (unsigned)UI_SCREEN_TABLE_COUNT, (unsigned)UI_T_COUNT, (unsigned)UI_LANG_COUNT);
    return failures;
}

int main(void)
{
    int failures = 0;
    failures += scenarios();
    failures += tables();
    failures += fuzz(0, 0);
    failures += fuzz(1, 0);
    failures += fuzz(0, 1);
    failures += fuzz(1, 1);
    if (failures == 0) {
        printf("ui_core: PASS\n");
        return 0;
    }
    printf("ui_core: FAIL (%d)\n", failures);
    return 1;
}
