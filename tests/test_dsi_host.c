/* =============================================================================
 * HYDRA-UMC Firmware - tests/test_dsi_host.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Host test of the DSI host of the CM7 (src/mcu_stm32h757/CM7/DsiHost.c) against a double of the HAL (tests/stubs/h7dsi): the numbers that reach the PLL (divisors, output divider
 * macro, escape clock under 20 MHz), the lanes, the video mode (colour coding, the sizes and the times of the plan, low-power options), the refusal of a bad plan or configuration
 * before the clock is touched, the undoing of a refused PLL or video mode, a second open, and the DCS writes (a short write for 0 or 1 parameter, a long write above that, a write
 * before the start or without parameters refused) together with the panel start-up of Common/panel_start.h running over them. Evidence: simulator.
 *
 *   cc -std=c11 -Wall -Wextra -DHYDRA_DSI_HOST=1 -Itests/stubs/h7dsi -Isrc/mcu_stm32h757/CM7 -Isrc/mcu_stm32h757/Common \
 *      -o test_dsi_host tests/test_dsi_host.c src/mcu_stm32h757/CM7/DsiHost.c
 */
#include <stdio.h>
#include <string.h>

#include "DsiHost.h"
#include "panel_init_table.h"
#include "panel_start.h"

#if !HYDRA_DSI_HOST
#error "this test is for the host with its switch on: build it with -DHYDRA_DSI_HOST=1"
#endif

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } } while (0)

DSI_TypeDef stub_dsi;

static int clock_on, clock_off;
static unsigned call_count;
static char calls[16];
static DSI_PLLInitTypeDef pll_seen;
static DSI_InitTypeDef init_seen;
static DSI_TypeDef *instance_seen;
static DSI_VidCfgTypeDef video_seen;
static int fail_init, fail_video, fail_start, fail_write;
static uint32_t short_mode, short_p1, short_p2, long_mode, long_n, long_p1;
static uint8_t long_data[8];
static unsigned shorts, longs;
static uint32_t delays[8];
static unsigned delay_count;

static void note(char c) { if (call_count < sizeof calls - 1u) calls[call_count++] = c; }

void stub_clock(int which) { if (which > 0) clock_on++; else clock_off++; }
HAL_StatusTypeDef HAL_DSI_Init(DSI_HandleTypeDef *h, DSI_PLLInitTypeDef *p) { note('I'); pll_seen = *p; init_seen = h->Init; instance_seen = h->Instance; return fail_init ? HAL_ERROR : HAL_OK; }
HAL_StatusTypeDef HAL_DSI_DeInit(DSI_HandleTypeDef *h) { (void)h; note('X'); return HAL_OK; }
HAL_StatusTypeDef HAL_DSI_ConfigVideoMode(DSI_HandleTypeDef *h, DSI_VidCfgTypeDef *v) { (void)h; note('V'); video_seen = *v; return fail_video ? HAL_ERROR : HAL_OK; }
HAL_StatusTypeDef HAL_DSI_Start(DSI_HandleTypeDef *h) { (void)h; note('S'); return fail_start ? HAL_ERROR : HAL_OK; }
HAL_StatusTypeDef HAL_DSI_ShortWrite(DSI_HandleTypeDef *h, uint32_t ch, uint32_t mode, uint32_t p1, uint32_t p2)
{
    (void)h; (void)ch;
    shorts++; short_mode = mode; short_p1 = p1; short_p2 = p2;
    return fail_write ? HAL_ERROR : HAL_OK;
}
HAL_StatusTypeDef HAL_DSI_LongWrite(DSI_HandleTypeDef *h, uint32_t ch, uint32_t mode, uint32_t n, uint32_t p1, uint8_t *data)
{
    (void)h; (void)ch;
    longs++; long_mode = mode; long_n = n; long_p1 = p1;
    memcpy(long_data, data, n < 8u ? n : 8u);
    return fail_write ? HAL_ERROR : HAL_OK;
}
void HAL_Delay(uint32_t ms) { if (delay_count < 8u) delays[delay_count] = ms; delay_count++; }

static void reset(void)
{
    clock_on = clock_off = 0;
    call_count = 0u;
    memset(calls, 0, sizeof calls);
    fail_init = fail_video = fail_start = fail_write = 0;
    shorts = longs = delay_count = 0u;
}

static void reset_line(void *ctx, int level) { (void)ctx; (void)level; }

int main(void)
{
    DsipConfig config = {25000000UL, 2u, 600000UL, 5u, 16u, 60u, 1280u, 720u, 4u, 60u, 60u, 4u, 16u, 16u};
    DsipPlan plan;
    CHECK(DsiPlan_Make(&config, &plan) == DSIP_OK, "a plan to start with");

    /* writes before the start fail */
    reset();
    CHECK(DsiHost_DcsWrite(NULL, 0x11u, NULL, 0u) == 1 && shorts == 0u, "a write before the start is refused");

    /* bad arguments, nothing touched */
    DsipConfig bad = config;
    bad.lanes = 3u;
    DsipPlan bad_plan = plan;
    bad_plan.idf = 0u;
    DsipPlan high_plan = plan;
    high_plan.idf = 8u;
    CHECK(DsiHost_Open(&config, NULL) == DSIH_E_ARG && DsiHost_Open(&bad, &plan) == DSIH_E_ARG && DsiHost_Open(NULL, &plan) == DSIH_E_ARG && clock_on == 0, "no plan, a configuration the plan refuses, none");
    CHECK(DsiHost_Open(&config, &bad_plan) == DSIH_E_ARG && DsiHost_Open(&config, &high_plan) == DSIH_E_ARG && clock_on == 0 && call_count == 0u, "an input divider outside 1..7");

    /* the PLL refused and the video mode refused: undone */
    reset(); fail_init = 1;
    CHECK(DsiHost_Open(&config, &plan) == DSIH_E_HAL && strcmp(calls, "I") == 0 && clock_off == 1, "a refused PLL: clock off");
    reset(); fail_video = 1;
    CHECK(DsiHost_Open(&config, &plan) == DSIH_E_HAL && strcmp(calls, "IVX") == 0 && clock_off == 1, "a refused video mode: released, clock off");
    reset(); fail_start = 1;
    CHECK(DsiHost_Open(&config, &plan) == DSIH_E_HAL && strcmp(calls, "IVSX") == 0 && clock_off == 1, "a refused start: released, clock off");
    CHECK(DsiHost_DcsWrite(NULL, 0x11u, NULL, 0u) == 1, "and the host is still closed");

    /* the good start */
    reset();
    CHECK(DsiHost_Open(&config, &plan) == DSIH_OK && strcmp(calls, "IVS") == 0 && clock_on == 1 && instance_seen == DSI, "a good start");
    CHECK(pll_seen.PLLNDIV == 24u && pll_seen.PLLIDF == DSI_PLL_IN_DIV1 && pll_seen.PLLODF == DSI_PLL_OUT_DIV1, "the divisors of the plan");
    CHECK(init_seen.NumberOfLanes == DSI_TWO_DATA_LANES && init_seen.AutomaticClockLaneControl == DSI_AUTO_CLK_LANE_CTRL_DISABLE && init_seen.TXEscapeCkdiv == 4u, "two lanes, and a byte clock of 75 MHz gives an escape divider of 4 (18.75 MHz)");
    CHECK(video_seen.ColorCoding == DSI_RGB565 && video_seen.Mode == DSI_VID_MODE_NB_PULSES && video_seen.PacketSize == 1280u && video_seen.NumberOfChunks == 0u && video_seen.NullPacketSize == 0xFFFu, "RGB565, sync pulses, one packet per line");
    CHECK(video_seen.HorizontalSyncActive == 5u && video_seen.HorizontalBackPorch == 71u && video_seen.HorizontalLine == 1653u, "the horizontal times in byte clocks");
    CHECK(video_seen.VerticalSyncActive == 4u && video_seen.VerticalBackPorch == 16u && video_seen.VerticalFrontPorch == 16u && video_seen.VerticalActive == 720u, "the vertical times in lines");
    CHECK(video_seen.LPCommandEnable == DSI_LP_COMMAND_ENABLE && video_seen.LPHorizontalFrontPorchEnable == DSI_LP_HFP_ENABLE && video_seen.LPHorizontalBackPorchEnable == DSI_LP_HBP_ENABLE && video_seen.LPVerticalActiveEnable == DSI_LP_VACT_ENABLE && video_seen.LPVerticalFrontPorchEnable == DSI_LP_VFP_ENABLE && video_seen.LPVerticalBackPorchEnable == DSI_LP_VBP_ENABLE && video_seen.LPVerticalSyncActiveEnable == DSI_LP_VSYNC_ENABLE && video_seen.FrameBTAAcknowledgeEnable == DSI_FBTAA_DISABLE, "every low-power option of the blanking on, no acknowledge");
    CHECK(video_seen.LPLargestPacketSize == 16u && video_seen.LPVACTLargestPacketSize == 0u && video_seen.VirtualChannelID == 0u && video_seen.LooselyPacked == DSI_LOOSELY_PACKED_DISABLE, "packet sizes and the channel");
    CHECK(video_seen.HSPolarity == DSI_HSYNC_ACTIVE_HIGH && video_seen.VSPolarity == DSI_VSYNC_ACTIVE_HIGH && video_seen.DEPolarity == DSI_DATA_ENABLE_ACTIVE_HIGH, "polarities");
    CHECK(DsiHost_Open(&config, &plan) == DSIH_E_STATE, "a second open is refused");

    /* the writes */
    CHECK(DsiHost_DcsWrite(NULL, 0x11u, NULL, 0u) == 0 && shorts == 1u && short_mode == DSI_DCS_SHORT_PKT_WRITE_P0 && short_p1 == 0x11u && short_p2 == 0u, "no parameter: a short write");
    const uint8_t one[1] = {0x55u};
    CHECK(DsiHost_DcsWrite(NULL, 0x3Au, one, 1u) == 0 && short_mode == DSI_DCS_SHORT_PKT_WRITE_P1 && short_p1 == 0x3Au && short_p2 == 0x55u, "one parameter: a short write with it");
    const uint8_t three[3] = {0x98u, 0x81u, 0x03u};
    CHECK(DsiHost_DcsWrite(NULL, 0xFFu, three, 3u) == 0 && longs == 1u && long_mode == DSI_DCS_LONG_PKT_WRITE && long_n == 3u && long_p1 == 0xFFu && long_data[0] == 0x98u && long_data[2] == 0x03u, "three parameters: a long write");
    CHECK(DsiHost_DcsWrite(NULL, 0x3Au, NULL, 1u) == 1 && DsiHost_DcsWrite(NULL, 0xFFu, NULL, 3u) == 1, "parameters announced but missing");
    fail_write = 1;
    CHECK(DsiHost_DcsWrite(NULL, 0x11u, NULL, 0u) == 1 && DsiHost_DcsWrite(NULL, 0x3Au, one, 1u) == 1 && DsiHost_DcsWrite(NULL, 0xFFu, three, 3u) == 1, "a host that fails is a failure");
    fail_write = 0;
    DsiHost_Delay(NULL, 120u);
    CHECK(delay_count == 1u && delays[0] == 120u, "the wait goes to the HAL");

    /* the panel start-up of the file over the host */
    shorts = longs = delay_count = 0u;
    const PanelOps ops = {reset_line, DsiHost_Delay, DsiHost_DcsWrite, NULL};
    const PanelTiming timing = {PANEL_INIT_RESET_LOW_MS, PANEL_INIT_RESET_HIGH_MS};
    CHECK(Panel_Start(&ops, &timing, g_panel_init, PANEL_INIT_STEPS, NULL) == PANEL_OK, "the panel starts over the host");
    CHECK(shorts == 2u && short_p1 == 0x29u && delay_count == 4u && delays[2] == 120u && delays[3] == 20u, "sleep-out, 120 ms, display-on, 20 ms");

    /* other colour codings and lanes in a fresh process are not possible (the host stays open); the mapping is read from a plan of 24 and 18 bits and one lane by the python test */

    if (failures) {
        printf("dsi_host host tests: %d FAILED\n", failures);
        return 1;
    }
    printf("dsi_host host logic tests passed\n");
    return 0;
}
