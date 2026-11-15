/* =============================================================================
 * HYDRA-UMC - src/mcu_stm32h757/CM7/DsiHost.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * DSI host of the CM7; see DsiHost.h, Common/dsi_plan.h and Common/panel_start.h.
 * ========================================================================== */
#include "DsiHost.h"

#if HYDRA_DSI_HOST

#define DSIH_ESCAPE_MAX_KHZ 20000u /* the transmit escape clock may not pass 20 MHz */

static DSI_HandleTypeDef g_dsi;
static int g_open;

static const uint32_t g_idf[8] = {0u, DSI_PLL_IN_DIV1, DSI_PLL_IN_DIV2, DSI_PLL_IN_DIV3, DSI_PLL_IN_DIV4, DSI_PLL_IN_DIV5, DSI_PLL_IN_DIV6, DSI_PLL_IN_DIV7};

static uint32_t DsiHost_OutputDivider(uint32_t odf)
{
    return odf == 1u ? DSI_PLL_OUT_DIV1 : (odf == 2u ? DSI_PLL_OUT_DIV2 : (odf == 4u ? DSI_PLL_OUT_DIV4 : DSI_PLL_OUT_DIV8));
}

static uint32_t DsiHost_ColorCoding(uint32_t bpp)
{
    return bpp == 16u ? DSI_RGB565 : (bpp == 18u ? DSI_RGB666 : DSI_RGB888);
}

int DsiHost_Open(const DsipConfig *config, const DsipPlan *plan)
{
    if (plan == NULL || DsiPlan_CheckConfig(config) != DSIP_OK) return DSIH_E_ARG;
    if (plan->idf < 1u || plan->idf > DSIP_IDF_MAX) return DSIH_E_ARG;
    if (g_open) return DSIH_E_STATE;
    __HAL_RCC_DSI_CLK_ENABLE();
    DSI_PLLInitTypeDef pll = {0};
    pll.PLLNDIV = plan->ndiv;
    pll.PLLIDF = g_idf[plan->idf];
    pll.PLLODF = DsiHost_OutputDivider(plan->odf);
    g_dsi.Instance = DSI;
    g_dsi.Init.AutomaticClockLaneControl = DSI_AUTO_CLK_LANE_CTRL_DISABLE;
    g_dsi.Init.TXEscapeCkdiv = (plan->byte_clock_khz + DSIH_ESCAPE_MAX_KHZ - 1u) / DSIH_ESCAPE_MAX_KHZ;
    g_dsi.Init.NumberOfLanes = config->lanes == 1u ? DSI_ONE_DATA_LANE : DSI_TWO_DATA_LANES;
    if (HAL_DSI_Init(&g_dsi, &pll) != HAL_OK) {
        __HAL_RCC_DSI_CLK_DISABLE();
        return DSIH_E_HAL;
    }

    DSI_VidCfgTypeDef video = {0};
    video.VirtualChannelID = 0u;
    video.ColorCoding = DsiHost_ColorCoding(config->bpp);
    video.LooselyPacked = DSI_LOOSELY_PACKED_DISABLE;
    video.Mode = DSI_VID_MODE_NB_PULSES;
    video.PacketSize = config->width;
    video.NumberOfChunks = 0u;
    video.NullPacketSize = 0xFFFu;
    video.HSPolarity = DSI_HSYNC_ACTIVE_HIGH;
    video.VSPolarity = DSI_VSYNC_ACTIVE_HIGH;
    video.DEPolarity = DSI_DATA_ENABLE_ACTIVE_HIGH;
    video.HorizontalSyncActive = plan->hsa;
    video.HorizontalBackPorch = plan->hbp;
    video.HorizontalLine = plan->hline;
    video.VerticalSyncActive = plan->vsa;
    video.VerticalBackPorch = plan->vbp;
    video.VerticalFrontPorch = plan->vfp;
    video.VerticalActive = plan->vact;
    video.LPCommandEnable = DSI_LP_COMMAND_ENABLE;
    video.LPLargestPacketSize = 16u;
    video.LPVACTLargestPacketSize = 0u;
    video.LPHorizontalFrontPorchEnable = DSI_LP_HFP_ENABLE;
    video.LPHorizontalBackPorchEnable = DSI_LP_HBP_ENABLE;
    video.LPVerticalActiveEnable = DSI_LP_VACT_ENABLE;
    video.LPVerticalFrontPorchEnable = DSI_LP_VFP_ENABLE;
    video.LPVerticalBackPorchEnable = DSI_LP_VBP_ENABLE;
    video.LPVerticalSyncActiveEnable = DSI_LP_VSYNC_ENABLE;
    video.FrameBTAAcknowledgeEnable = DSI_FBTAA_DISABLE;
    if (HAL_DSI_ConfigVideoMode(&g_dsi, &video) != HAL_OK || HAL_DSI_Start(&g_dsi) != HAL_OK) {
        (void)HAL_DSI_DeInit(&g_dsi);
        __HAL_RCC_DSI_CLK_DISABLE();
        return DSIH_E_HAL;
    }
    g_open = 1;
    return DSIH_OK;
}

int DsiHost_DcsWrite(void *ctx, uint8_t command, const uint8_t *params, uint8_t count)
{
    (void)ctx;
    if (!g_open) return 1;
    if (count == 0u) return HAL_DSI_ShortWrite(&g_dsi, 0u, DSI_DCS_SHORT_PKT_WRITE_P0, command, 0u) == HAL_OK ? 0 : 1;
    if (params == NULL) return 1;
    if (count == 1u) return HAL_DSI_ShortWrite(&g_dsi, 0u, DSI_DCS_SHORT_PKT_WRITE_P1, command, params[0]) == HAL_OK ? 0 : 1;
    return HAL_DSI_LongWrite(&g_dsi, 0u, DSI_DCS_LONG_PKT_WRITE, count, command, (uint8_t *)(uintptr_t)params) == HAL_OK ? 0 : 1;
}

void DsiHost_Delay(void *ctx, uint32_t ms)
{
    (void)ctx;
    HAL_Delay(ms);
}

#endif /* HYDRA_DSI_HOST */
