/* =============================================================================
 * HYDRA-UMC Firmware - tests/test_panel_start.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Host test of the start-up sequence of the display panel (src/mcu_stm32h757/Common/panel_start.h) with the table made from config/panel.cfg (Common/panel_init_table.h): a recording
 * double plays the reset line, the waits and the DCS writes, and the test checks the order (reset low, wait, reset high, wait, then the commands each with its wait), the table
 * of the file, the refusal of a bad table or bad timing before anything is touched (no sleep-out, no display-on, display-on first, a wait under 120 ms after the sleep-out, too many
 * steps or parameters, a wait over 1000 ms, a missing parameter pointer), and a host that refuses a command (the sequence stops and the step number is given). Evidence: simulator.
 *
 *   cc -std=c11 -Wall -Wextra -Isrc/mcu_stm32h757/Common -o test_panel_start tests/test_panel_start.c
 */
#include <stdio.h>
#include <string.h>

#include "panel_init_table.h"
#include "panel_start.h"

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } } while (0)

static char log_text[256];
static unsigned log_length;
static unsigned writes, fail_at_write; /* fail_at_write: the write (1-based) that fails; 0 = none */

static void note(const char *text)
{
    const unsigned n = (unsigned)strlen(text);
    if (log_length + n < sizeof log_text - 1u) { memcpy(log_text + log_length, text, n + 1u); log_length += n; }
}

static void reset_line(void *ctx, int level) { (void)ctx; note(level ? "R1 " : "R0 "); }
static void delay_ms(void *ctx, uint32_t ms) { (void)ctx; char text[16]; snprintf(text, sizeof text, "W%lu ", (unsigned long)ms); note(text); }
static int dcs_write(void *ctx, uint8_t command, const uint8_t *params, uint8_t count)
{
    (void)ctx;
    char text[32];
    snprintf(text, sizeof text, "C%02X/%u%s ", command, count, (count && params) ? "p" : "");
    note(text);
    writes++;
    return (fail_at_write && writes == fail_at_write) ? 1 : 0;
}

static void clear(void)
{
    memset(log_text, 0, sizeof log_text);
    log_length = 0u;
    writes = 0u;
    fail_at_write = 0u;
}

int main(void)
{
    const PanelOps ops = {reset_line, delay_ms, dcs_write, NULL};
    const PanelTiming timing = {PANEL_INIT_RESET_LOW_MS, PANEL_INIT_RESET_HIGH_MS};
    uint32_t failed = 0xFFFFu;

    /* the table of the file */
    clear();
    CHECK(PANEL_INIT_STEPS == 2u && PANEL_INIT_RESET_LOW_MS == 10u && PANEL_INIT_RESET_HIGH_MS == 120u, "the starting table has two steps");
    CHECK(Panel_CheckTable(g_panel_init, PANEL_INIT_STEPS) == PANEL_OK, "and it is a good table");
    CHECK(Panel_Start(&ops, &timing, g_panel_init, PANEL_INIT_STEPS, &failed) == PANEL_OK && failed == 0xFFFFu, "it starts");
    CHECK(strcmp(log_text, "R0 W10 R1 W120 C11/0 W120 C29/0 W20 ") == 0, "reset low, wait, reset high, wait, sleep-out and its 120 ms, display-on and its 20 ms");

    /* a table with parameters and a step without a wait */
    static const uint8_t page[] = {0x98, 0x81, 0x03};
    static const uint8_t one[] = {0x55};
    const PanelStep good[] = {{0xFF, 3, page, 0}, {0x3A, 1, one, 5}, {PANEL_SLEEP_OUT, 0, NULL, 120}, {PANEL_DISPLAY_ON, 0, NULL, 0}};
    clear();
    CHECK(Panel_Start(&ops, &timing, good, 4u, NULL) == PANEL_OK && strcmp(log_text, "R0 W10 R1 W120 CFF/3p C3A/1p W5 C11/0 W120 C29/0 ") == 0, "parameters are passed, a wait of 0 is skipped");

    /* a host that refuses a command */
    clear(); fail_at_write = 2u;
    CHECK(Panel_Start(&ops, &timing, good, 4u, &failed) == PANEL_E_WRITE && failed == 1u && strstr(log_text, "C11") == NULL, "the second command refused: the sequence stops and says which");
    clear(); fail_at_write = 1u;
    CHECK(Panel_Start(&ops, &timing, good, 4u, NULL) == PANEL_E_WRITE, "a refusal without a place to report it");

    /* tables that are refused before anything is touched */
    clear();
    const PanelStep no_sleep[] = {{PANEL_DISPLAY_ON, 0, NULL, 0}};
    const PanelStep no_display[] = {{PANEL_SLEEP_OUT, 0, NULL, 120}};
    const PanelStep wrong_order[] = {{PANEL_DISPLAY_ON, 0, NULL, 0}, {PANEL_SLEEP_OUT, 0, NULL, 120}};
    const PanelStep short_sleep[] = {{PANEL_SLEEP_OUT, 0, NULL, 119}, {PANEL_DISPLAY_ON, 0, NULL, 0}};
    const PanelStep long_wait[] = {{PANEL_SLEEP_OUT, 0, NULL, 120}, {PANEL_DISPLAY_ON, 0, NULL, 1001}};
    const PanelStep many_params[] = {{0xFF, 65, page, 0}, {PANEL_SLEEP_OUT, 0, NULL, 120}, {PANEL_DISPLAY_ON, 0, NULL, 0}};
    const PanelStep missing_params[] = {{0xFF, 2, NULL, 0}, {PANEL_SLEEP_OUT, 0, NULL, 120}, {PANEL_DISPLAY_ON, 0, NULL, 0}};
    CHECK(Panel_Start(&ops, &timing, no_sleep, 1u, NULL) == PANEL_E_SEQUENCE, "no sleep-out");
    CHECK(Panel_Start(&ops, &timing, no_display, 1u, NULL) == PANEL_E_SEQUENCE, "no display-on");
    CHECK(Panel_Start(&ops, &timing, wrong_order, 2u, NULL) == PANEL_E_SEQUENCE, "display-on before the sleep-out");
    CHECK(Panel_Start(&ops, &timing, short_sleep, 2u, NULL) == PANEL_E_SLEEP, "119 ms after the sleep-out");
    CHECK(Panel_Start(&ops, &timing, long_wait, 2u, NULL) == PANEL_E_TIMING, "a wait over 1000 ms");
    CHECK(Panel_Start(&ops, &timing, many_params, 3u, NULL) == PANEL_E_TABLE, "65 parameters");
    CHECK(Panel_Start(&ops, &timing, missing_params, 3u, NULL) == PANEL_E_TABLE, "parameters announced but no pointer");
    CHECK(Panel_Start(&ops, &timing, good, 0u, NULL) == PANEL_E_TABLE && Panel_Start(&ops, &timing, good, 129u, NULL) == PANEL_E_TABLE, "no steps, too many steps");
    CHECK(Panel_Start(&ops, &timing, NULL, 2u, NULL) == PANEL_E_ARG && Panel_CheckTable(NULL, 1u) == PANEL_E_ARG, "no table");
    CHECK(log_length == 0u && writes == 0u, "nothing was touched by any refusal");

    /* the limits that are accepted: 128 steps, 64 parameters, 1000 ms */
    static PanelStep big[PANEL_STEPS_MAX];
    static const uint8_t sixty_four[64] = {0};
    for (unsigned i = 0u; i < PANEL_STEPS_MAX; i++) { big[i].command = 0xB0; big[i].count = 64; big[i].params = sixty_four; big[i].delay_ms = 1000; }
    big[0].command = PANEL_SLEEP_OUT; big[0].count = 0; big[0].params = NULL; big[0].delay_ms = 120;
    big[127].command = PANEL_DISPLAY_ON; big[127].count = 0; big[127].params = NULL;
    CHECK(Panel_CheckTable(big, 128u) == PANEL_OK, "128 steps of 64 parameters and 1000 ms are accepted");

    /* the timing and the operations */
    const PanelTiming zero_low = {0u, 10u}, big_low = {1001u, 10u}, zero_high = {10u, 0u}, big_high = {10u, 1001u}, edge = {1u, 1000u};
    CHECK(Panel_Start(&ops, &zero_low, good, 4u, NULL) == PANEL_E_TIMING && Panel_Start(&ops, &big_low, good, 4u, NULL) == PANEL_E_TIMING, "the reset low time is 1..1000");
    CHECK(Panel_Start(&ops, &zero_high, good, 4u, NULL) == PANEL_E_TIMING && Panel_Start(&ops, &big_high, good, 4u, NULL) == PANEL_E_TIMING, "the reset high time is 1..1000");
    CHECK(log_length == 0u, "nothing was touched");
    CHECK(Panel_Start(&ops, &edge, good, 4u, NULL) == PANEL_OK, "the edges of the reset time are accepted");
    const PanelOps no_reset = {NULL, delay_ms, dcs_write, NULL}, no_delay = {reset_line, NULL, dcs_write, NULL}, no_write = {reset_line, delay_ms, NULL, NULL};
    CHECK(Panel_Start(&no_reset, &timing, good, 4u, NULL) == PANEL_E_ARG && Panel_Start(&no_delay, &timing, good, 4u, NULL) == PANEL_E_ARG && Panel_Start(&no_write, &timing, good, 4u, NULL) == PANEL_E_ARG, "all three operations are needed");
    CHECK(Panel_Start(NULL, &timing, good, 4u, NULL) == PANEL_E_ARG && Panel_Start(&ops, NULL, good, 4u, NULL) == PANEL_E_ARG, "no operations, no timing");

    if (failures) {
        printf("panel_start host tests: %d FAILED\n", failures);
        return 1;
    }
    printf("panel_start host logic tests passed\n");
    return 0;
}
