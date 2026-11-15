/* =============================================================================
 * HYDRA-UMC Firmware - src/mcu_stm32h757/Common/panel_start.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * panel_start.h - the start-up sequence of the DSI panel (ILI9881) of the H757 board (HAL-free, host-testable): the reset pulse of the panel, then the commands of an init table in
 * the order they are given, each followed by its own wait. The reset line, the wait and the DCS write are reached only through PanelOps (the DSI host and a GPIO on the board, a double
 * in tests/test_panel_start.c). The table is checked before anything is touched: it has to have between 1 and 128 steps of at most 64 parameters, no wait over 1000 ms, a sleep-out
 * (0x11) followed by a wait of at least 120 ms (the panel needs that time before it takes another command), and a display-on (0x29) that comes after the sleep-out; a table without
 * them, or with the display switched on before the sleep-out, is refused and the panel is not reset. The first command that the host cannot send stops the sequence and its number is
 * given. The table of config/panel.cfg is in Common/panel_init_table.h (made by tools/dsi_plan.py). Nothing here has run on a board.
 * ============================================================================= */
#ifndef HYDRA_UMC_PANEL_START_H
#define HYDRA_UMC_PANEL_START_H

#include <stddef.h>
#include <stdint.h>

#define PANEL_STEPS_MAX 128u
#define PANEL_PARAMS_MAX 64u
#define PANEL_DELAY_MAX_MS 1000u
#define PANEL_RESET_MIN_MS 1u
#define PANEL_RESET_MAX_MS 1000u
#define PANEL_SLEEP_OUT 0x11u
#define PANEL_DISPLAY_ON 0x29u
#define PANEL_SLEEP_OUT_MIN_DELAY_MS 120u

#define PANEL_OK 0
#define PANEL_E_ARG 1
#define PANEL_E_TIMING 2   /* the reset pulse or a wait is outside its range */
#define PANEL_E_TABLE 3    /* no steps, too many, or too many parameters in one */
#define PANEL_E_SEQUENCE 4 /* no sleep-out, no display-on, or the display-on first */
#define PANEL_E_SLEEP 5    /* the wait after the sleep-out is under 120 ms */
#define PANEL_E_WRITE 6    /* the host could not send a command */

typedef struct {
    uint8_t command;
    uint8_t count;
    const uint8_t *params;
    uint16_t delay_ms; /* the wait after this command */
} PanelStep;

typedef struct {
    void (*reset_line)(void *ctx, int level); /* 0 = the panel is held in reset */
    void (*delay_ms)(void *ctx, uint32_t ms);
    int (*dcs_write)(void *ctx, uint8_t command, const uint8_t *params, uint8_t count); /* 0 on success */
    void *ctx;
} PanelOps;

typedef struct {
    uint32_t reset_low_ms, reset_high_ms;
} PanelTiming;

static inline int Panel_CheckTable(const PanelStep *steps, uint32_t count)
{
    if (!steps) return PANEL_E_ARG;
    if (count < 1u || count > PANEL_STEPS_MAX) return PANEL_E_TABLE;
    int sleep_at = -1, display_at = -1;
    for (uint32_t i = 0u; i < count; i++) {
        if (steps[i].count > PANEL_PARAMS_MAX || (steps[i].count > 0u && !steps[i].params)) return PANEL_E_TABLE;
        if (steps[i].delay_ms > PANEL_DELAY_MAX_MS) return PANEL_E_TIMING;
        if (steps[i].command == PANEL_SLEEP_OUT && sleep_at < 0) {
            sleep_at = (int)i;
            if (steps[i].delay_ms < PANEL_SLEEP_OUT_MIN_DELAY_MS) return PANEL_E_SLEEP;
        }
        if (steps[i].command == PANEL_DISPLAY_ON && display_at < 0) display_at = (int)i;
    }
    if (sleep_at < 0 || display_at < 0 || display_at < sleep_at) return PANEL_E_SEQUENCE;
    return PANEL_OK;
}

/* Resets the panel and sends the table. On PANEL_E_WRITE `*failed` (if given) is the number of the step that the host refused. */
static inline int Panel_Start(const PanelOps *ops, const PanelTiming *timing, const PanelStep *steps, uint32_t count, uint32_t *failed)
{
    if (!ops || !ops->reset_line || !ops->delay_ms || !ops->dcs_write || !timing) return PANEL_E_ARG;
    if (timing->reset_low_ms < PANEL_RESET_MIN_MS || timing->reset_low_ms > PANEL_RESET_MAX_MS || timing->reset_high_ms < PANEL_RESET_MIN_MS || timing->reset_high_ms > PANEL_RESET_MAX_MS)
        return PANEL_E_TIMING;
    const int rc = Panel_CheckTable(steps, count);
    if (rc != PANEL_OK) return rc;

    ops->reset_line(ops->ctx, 0);
    ops->delay_ms(ops->ctx, timing->reset_low_ms);
    ops->reset_line(ops->ctx, 1);
    ops->delay_ms(ops->ctx, timing->reset_high_ms);
    for (uint32_t i = 0u; i < count; i++) {
        if (ops->dcs_write(ops->ctx, steps[i].command, steps[i].params, steps[i].count) != 0) {
            if (failed) *failed = i;
            return PANEL_E_WRITE;
        }
        if (steps[i].delay_ms) ops->delay_ms(ops->ctx, steps[i].delay_ms);
    }
    return PANEL_OK;
}

#endif /* HYDRA_UMC_PANEL_START_H */
