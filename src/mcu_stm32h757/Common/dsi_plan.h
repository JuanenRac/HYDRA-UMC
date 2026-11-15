/* =============================================================================
 * HYDRA-UMC Firmware - src/mcu_stm32h757/Common/dsi_plan.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * dsi_plan.h - the numbers that the DSI host of the H757 needs to send the picture of the LTDC to the panel (HAL-free, host-testable). From the crystal, the number of lanes and the
 * lane rate asked for it works out the divisors of the DSI PLL (input divider 1 to 7, multiplier 10 to 125, output divider 1, 2, 4 or 8) so that the lane rate comes out within a
 * tolerance, the VCO stays between 1 and 2 GHz and the input of the PLL between 4 and 25 MHz; among the ways to get the best rate it takes the one with the lowest VCO. From the size
 * and the porches of the picture (config/display.cfg) and the refresh rate it works out the pixel clock and turns the horizontal times from pixel clocks into lane byte clocks, the
 * unit of the video registers of the host. A picture that the lanes cannot carry (the pixel data plus 15 percent for the packets must fit the lanes) or a time that does not fit its
 * register is refused. The panel start-up is Common/panel_start.h; tools/dsi_plan.py is the Python twin and reads config/panel.cfg. Nothing here has run on a board.
 * ============================================================================= */
#ifndef HYDRA_UMC_DSI_PLAN_H
#define HYDRA_UMC_DSI_PLAN_H

#include <stdint.h>

#define DSIP_HSE_MIN_HZ 4000000UL
#define DSIP_HSE_MAX_HZ 50000000UL
#define DSIP_PLL_IN_MIN_KHZ 4000UL
#define DSIP_PLL_IN_MAX_KHZ 25000UL
#define DSIP_VCO_MIN_KHZ 1000000UL
#define DSIP_VCO_MAX_KHZ 2000000UL
#define DSIP_IDF_MAX 7UL
#define DSIP_NDIV_MIN 10UL
#define DSIP_NDIV_MAX 125UL
#define DSIP_LANE_MIN_KBPS 62500UL
#define DSIP_LANE_MAX_KBPS 1000000UL
#define DSIP_TOLERANCE_MAX_PERMILLE 50UL
#define DSIP_LANES_MAX 2UL /* the number of lanes is 1 or 2 on the host of the H7 */
#define DSIP_SIZE_MAX 2048UL
#define DSIP_PORCH_MAX 1024UL
#define DSIP_FPS_MIN 10UL
#define DSIP_FPS_MAX 120UL
#define DSIP_HSA_HBP_MAX 4095UL    /* the horizontal sync and back porch registers have 12 bits */
#define DSIP_HLINE_MAX 32767UL     /* the line register has 15 bits */
#define DSIP_PACKET_OVERHEAD_PERMILLE 150UL

#define DSIP_OK 0
#define DSIP_E_ARG 1
#define DSIP_E_HSE 2
#define DSIP_E_LANES 3
#define DSIP_E_LANE_RATE 4
#define DSIP_E_TOLERANCE 5
#define DSIP_E_BPP 6
#define DSIP_E_PICTURE 7  /* size, porch or refresh rate out of range */
#define DSIP_E_PLL 8      /* no divisors give the lane rate inside the tolerance */
#define DSIP_E_BANDWIDTH 9
#define DSIP_E_REGISTER 10 /* a time does not fit its register */

typedef struct {
    uint32_t hse_hz, lanes, lane_kbps, tolerance_permille, bpp, fps;
    uint32_t width, height, hsync, hbp, hfp, vsync, vbp, vfp;
} DsipConfig;

typedef struct {
    uint32_t idf, ndiv, odf;
    uint32_t vco_khz, lane_kbps, byte_clock_khz, pixel_khz;
    uint32_t hsa, hbp, hline; /* in lane byte clocks */
    uint32_t vsa, vbp, vfp, vact; /* in lines */
    uint32_t need_kbps;      /* what the picture needs on all the lanes together, packets included */
} DsipPlan;

static inline int DsiPlan_CheckConfig(const DsipConfig *c)
{
    if (!c) return DSIP_E_ARG;
    if (c->hse_hz < DSIP_HSE_MIN_HZ || c->hse_hz > DSIP_HSE_MAX_HZ) return DSIP_E_HSE;
    if (c->lanes < 1u || c->lanes > DSIP_LANES_MAX) return DSIP_E_LANES;
    if (c->lane_kbps < DSIP_LANE_MIN_KBPS || c->lane_kbps > DSIP_LANE_MAX_KBPS) return DSIP_E_LANE_RATE;
    if (c->tolerance_permille > DSIP_TOLERANCE_MAX_PERMILLE) return DSIP_E_TOLERANCE;
    if (c->bpp != 16u && c->bpp != 18u && c->bpp != 24u) return DSIP_E_BPP;
    if (c->fps < DSIP_FPS_MIN || c->fps > DSIP_FPS_MAX) return DSIP_E_PICTURE;
    if (c->width < 1u || c->width > DSIP_SIZE_MAX || c->height < 1u || c->height > DSIP_SIZE_MAX) return DSIP_E_PICTURE;
    if (c->hsync < 1u || c->hsync > DSIP_PORCH_MAX || c->hbp < 1u || c->hbp > DSIP_PORCH_MAX || c->hfp < 1u || c->hfp > DSIP_PORCH_MAX) return DSIP_E_PICTURE;
    if (c->vsync < 1u || c->vsync > DSIP_PORCH_MAX || c->vbp < 1u || c->vbp > DSIP_PORCH_MAX || c->vfp < 1u || c->vfp > DSIP_PORCH_MAX) return DSIP_E_PICTURE;
    return DSIP_OK;
}

/* The ratio of a time in pixel clocks to lane byte clocks, to the nearest. */
static inline uint32_t DsiPlan_ToByteClocks(uint32_t pixels, uint32_t byte_clock_khz, uint32_t pixel_khz)
{
    return (uint32_t)(((uint64_t)pixels * byte_clock_khz + pixel_khz / 2u) / pixel_khz);
}

/* Returns DSIP_OK or the first error; `plan` is written only on success. */
static inline int DsiPlan_Make(const DsipConfig *c, DsipPlan *plan)
{
    int rc = DsiPlan_CheckConfig(c);
    if (rc != DSIP_OK) return rc;
    if (!plan) return DSIP_E_ARG;

    const uint64_t hse_khz = c->hse_hz / 1000u;
    static const uint32_t odfs[4] = {1u, 2u, 4u, 8u};
    int found = 0;
    uint64_t best_error = 0u, best_vco = 0u;
    uint32_t best_idf = 0u, best_ndiv = 0u, best_odf = 0u;
    uint64_t best_rate = 0u;
    for (uint32_t idf = 1u; idf <= DSIP_IDF_MAX; idf++) {
        const uint64_t input_khz = hse_khz / idf;
        if (input_khz < DSIP_PLL_IN_MIN_KHZ || input_khz > DSIP_PLL_IN_MAX_KHZ) continue;
        for (uint32_t k = 0u; k < 4u; k++) {
            for (uint32_t ndiv = DSIP_NDIV_MIN; ndiv <= DSIP_NDIV_MAX; ndiv++) {
                const uint64_t vco = hse_khz * 2u * ndiv / idf;
                if (vco < DSIP_VCO_MIN_KHZ || vco > DSIP_VCO_MAX_KHZ) continue;
                const uint64_t rate = vco / (2u * odfs[k]);
                const uint64_t error = rate > c->lane_kbps ? rate - c->lane_kbps : c->lane_kbps - rate;
                if (error * 1000u > (uint64_t)c->lane_kbps * c->tolerance_permille) continue;
                if (!found || error < best_error || (error == best_error && vco < best_vco)) {
                    found = 1;
                    best_error = error;
                    best_vco = vco;
                    best_idf = idf;
                    best_ndiv = ndiv;
                    best_odf = odfs[k];
                    best_rate = rate;
                }
            }
        }
    }
    if (!found) return DSIP_E_PLL;

    const uint64_t htotal = (uint64_t)c->width + c->hsync + c->hbp + c->hfp;
    const uint64_t vtotal = (uint64_t)c->height + c->vsync + c->vbp + c->vfp;
    const uint64_t pixel_khz = (htotal * vtotal * c->fps + 500u) / 1000u;
    const uint64_t need_kbps = pixel_khz * c->bpp * (1000u + DSIP_PACKET_OVERHEAD_PERMILLE) / 1000u;
    if (need_kbps > best_rate * c->lanes) return DSIP_E_BANDWIDTH;

    const uint32_t byte_khz = (uint32_t)(best_rate / 8u);
    const uint32_t hsa = DsiPlan_ToByteClocks(c->hsync, byte_khz, (uint32_t)pixel_khz);
    const uint32_t hbp = DsiPlan_ToByteClocks(c->hbp, byte_khz, (uint32_t)pixel_khz);
    const uint32_t hline = DsiPlan_ToByteClocks((uint32_t)htotal, byte_khz, (uint32_t)pixel_khz);
    if (hsa > DSIP_HSA_HBP_MAX || hbp > DSIP_HSA_HBP_MAX || hline > DSIP_HLINE_MAX) return DSIP_E_REGISTER;

    plan->idf = best_idf;
    plan->ndiv = best_ndiv;
    plan->odf = best_odf;
    plan->vco_khz = (uint32_t)best_vco;
    plan->lane_kbps = (uint32_t)best_rate;
    plan->byte_clock_khz = byte_khz;
    plan->pixel_khz = (uint32_t)pixel_khz;
    plan->hsa = hsa;
    plan->hbp = hbp;
    plan->hline = hline;
    plan->vsa = c->vsync;
    plan->vbp = c->vbp;
    plan->vfp = c->vfp;
    plan->vact = c->height;
    plan->need_kbps = (uint32_t)need_kbps;
    return DSIP_OK;
}

#endif /* HYDRA_UMC_DSI_PLAN_H */
