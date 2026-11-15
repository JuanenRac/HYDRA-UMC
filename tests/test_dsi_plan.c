/* =============================================================================
 * HYDRA-UMC Firmware - tests/test_dsi_plan.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Host test of the numbers of the DSI link (src/mcu_stm32h757/Common/dsi_plan.h): literal plans for five cases (the same ones as tests/test_dsi_plan.py against the Python twin),
 * the choice of the PLL divisors (the best rate inside the tolerance, then the lowest VCO), the conversion of the horizontal times into lane byte clocks, the check of the bandwidth
 * of the lanes, and the refusal of every number out of range with its own code (nothing is written to the plan in a refusal). A sweep checks, for rates from 62.5 to 1000 Mbit/s,
 * that every plan has its VCO and the input of its PLL inside the limits. Evidence: simulator.
 *
 *   cc -std=c11 -Wall -Wextra -Isrc/mcu_stm32h757/Common -o test_dsi_plan tests/test_dsi_plan.c
 */
#include <stdio.h>
#include <string.h>

#include "dsi_plan.h"

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } } while (0)

static DsipConfig base(void)
{
    DsipConfig c = {25000000UL, 2u, 600000UL, 5u, 16u, 60u, 1280u, 720u, 4u, 60u, 60u, 4u, 16u, 16u};
    return c;
}

int main(void)
{
    DsipPlan p;
    DsipConfig c = base();

    /* the plans */
    CHECK(DsiPlan_Make(&c, &p) == DSIP_OK && p.idf == 1u && p.ndiv == 24u && p.odf == 1u && p.vco_khz == 1200000UL && p.lane_kbps == 600000UL, "the defaults: 600 Mbit/s from a VCO of 1.2 GHz");
    CHECK(p.byte_clock_khz == 75000UL && p.pixel_khz == 63685UL && p.hsa == 5u && p.hbp == 71u && p.hline == 1653u && p.need_kbps == 1171804UL, "byte clock, pixel clock and the horizontal times in byte clocks");
    CHECK(p.vsa == 4u && p.vbp == 16u && p.vfp == 16u && p.vact == 720u, "the vertical times stay in lines");
    c = base(); c.lane_kbps = 900000UL;
    CHECK(DsiPlan_Make(&c, &p) == DSIP_OK && p.ndiv == 36u && p.vco_khz == 1800000UL && p.byte_clock_khz == 112500UL && p.hsa == 7u && p.hbp == 106u && p.hline == 2480u, "900 Mbit/s");
    c = base(); c.hse_hz = 24000000UL;
    CHECK(DsiPlan_Make(&c, &p) == DSIP_OK && p.idf == 1u && p.ndiv == 25u && p.lane_kbps == 600000UL, "a 24 MHz crystal");
    c = base(); c.lane_kbps = 613000UL;
    CHECK(DsiPlan_Make(&c, &p) == DSIP_OK && p.idf == 2u && p.ndiv == 49u && p.lane_kbps == 612500UL && p.byte_clock_khz == 76562UL && p.hline == 1688u, "the nearest rate inside the tolerance");
    c = base(); c.lanes = 1u; c.lane_kbps = 1000000UL; c.width = 800u; c.height = 480u; c.hsync = 4u; c.hbp = 20u; c.hfp = 20u; c.vsync = 2u; c.vbp = 10u; c.vfp = 10u; c.fps = 30u;
    CHECK(DsiPlan_Make(&c, &p) == DSIP_OK && p.ndiv == 40u && p.vco_khz == 2000000UL && p.hsa == 39u && p.hbp == 197u && p.hline == 8300u && p.need_kbps == 233882UL, "one lane at 1 Gbit/s, 800 x 480");

    /* a plan that does not fit is not written */
    memset(&p, 0x5A, sizeof p);
    c = base(); c.lane_kbps = 613333UL; c.tolerance_permille = 0u;
    CHECK(DsiPlan_Make(&c, &p) == DSIP_E_PLL && p.idf == 0x5A5A5A5AUL, "no divisors give 613333 kbit/s exactly");
    c = base(); c.bpp = 24u;
    CHECK(DsiPlan_Make(&c, &p) == DSIP_E_BANDWIDTH && p.idf == 0x5A5A5A5AUL, "24 bits per pixel do not fit two lanes at 600 Mbit/s");
    c = base(); c.lane_kbps = 62500UL; c.width = 64u; c.height = 64u; c.hsync = 1024u; c.hbp = 1u; c.hfp = 1u; c.vsync = 1u; c.vbp = 1u; c.vfp = 1u; c.fps = 10u;
    CHECK(DsiPlan_Make(&c, &p) == DSIP_E_REGISTER && p.idf == 0x5A5A5A5AUL, "a sync time over the 12 bits of its register");
    c = base(); c.lane_kbps = 62500UL; c.width = 64u; c.height = 64u; c.hsync = 1u; c.hbp = 1u; c.hfp = 1u; c.vsync = 1u; c.vbp = 1u; c.vfp = 1u; c.fps = 10u;
    CHECK(DsiPlan_Make(&c, &p) == DSIP_OK && p.odf == 8u && p.ndiv == 20u && p.idf == 1u && p.hline == 11631u, "the lowest rate with the largest output divider");

    /* every number out of range, one code each */
    c = base(); c.hse_hz = 3999999UL;  CHECK(DsiPlan_Make(&c, &p) == DSIP_E_HSE, "crystal under");
    c = base(); c.hse_hz = 50000001UL; CHECK(DsiPlan_Make(&c, &p) == DSIP_E_HSE, "crystal over");
    c = base(); c.lanes = 0u;          CHECK(DsiPlan_Make(&c, &p) == DSIP_E_LANES, "no lanes");
    c = base(); c.lanes = 3u;          CHECK(DsiPlan_Make(&c, &p) == DSIP_E_LANES, "three lanes: the host has two");
    c = base(); c.lane_kbps = 62499UL; CHECK(DsiPlan_Make(&c, &p) == DSIP_E_LANE_RATE, "rate under");
    c = base(); c.lane_kbps = 1000001UL; CHECK(DsiPlan_Make(&c, &p) == DSIP_E_LANE_RATE, "rate over");
    c = base(); c.tolerance_permille = 51u; CHECK(DsiPlan_Make(&c, &p) == DSIP_E_TOLERANCE, "tolerance over");
    c = base(); c.bpp = 8u;            CHECK(DsiPlan_Make(&c, &p) == DSIP_E_BPP, "8 bits per pixel");
    c = base(); c.fps = 9u;            CHECK(DsiPlan_Make(&c, &p) == DSIP_E_PICTURE, "refresh rate under");
    c = base(); c.fps = 121u;          CHECK(DsiPlan_Make(&c, &p) == DSIP_E_PICTURE, "refresh rate over");
    c = base(); c.width = 0u;          CHECK(DsiPlan_Make(&c, &p) == DSIP_E_PICTURE, "no width");
    c = base(); c.width = 2049u;       CHECK(DsiPlan_Make(&c, &p) == DSIP_E_PICTURE, "width over");
    c = base(); c.height = 0u;         CHECK(DsiPlan_Make(&c, &p) == DSIP_E_PICTURE, "no height");
    c = base(); c.height = 2049u;      CHECK(DsiPlan_Make(&c, &p) == DSIP_E_PICTURE, "height over");
    c = base(); c.hsync = 0u;          CHECK(DsiPlan_Make(&c, &p) == DSIP_E_PICTURE, "no horizontal sync");
    c = base(); c.hbp = 1025u;         CHECK(DsiPlan_Make(&c, &p) == DSIP_E_PICTURE, "horizontal back porch over");
    c = base(); c.hfp = 0u;            CHECK(DsiPlan_Make(&c, &p) == DSIP_E_PICTURE, "no horizontal front porch");
    c = base(); c.vsync = 1025u;       CHECK(DsiPlan_Make(&c, &p) == DSIP_E_PICTURE, "vertical sync over");
    c = base(); c.vbp = 0u;            CHECK(DsiPlan_Make(&c, &p) == DSIP_E_PICTURE, "no vertical back porch");
    c = base(); c.vfp = 1025u;         CHECK(DsiPlan_Make(&c, &p) == DSIP_E_PICTURE, "vertical front porch over");
    CHECK(DsiPlan_Make(NULL, &p) == DSIP_E_ARG && DsiPlan_CheckConfig(NULL) == DSIP_E_ARG, "no configuration");
    c = base();
    CHECK(DsiPlan_Make(&c, NULL) == DSIP_E_ARG, "no output");

    /* the sweep */
    unsigned made = 0u;
    for (uint32_t rate = 62500UL; rate <= 1000000UL; rate += 12500UL) {
        c = base();
        c.lane_kbps = rate; c.tolerance_permille = 50u; c.lanes = 2u; c.width = 320u; c.height = 240u; c.fps = 30u;
        if (DsiPlan_Make(&c, &p) != DSIP_OK) continue;
        made++;
        const uint64_t error = p.lane_kbps > rate ? p.lane_kbps - rate : rate - p.lane_kbps;
        CHECK(p.vco_khz >= DSIP_VCO_MIN_KHZ && p.vco_khz <= DSIP_VCO_MAX_KHZ && 25000UL / p.idf >= 4u * 1u && error * 1000u <= (uint64_t)rate * 50u, "a swept plan keeps the VCO, the PLL input and the tolerance");
        CHECK(p.lane_kbps == p.vco_khz / (2u * p.odf) && p.ndiv >= 10u && p.ndiv <= 125u && (p.odf == 1u || p.odf == 2u || p.odf == 4u || p.odf == 8u), "and the divisors in range");
    }
    CHECK(made > 40u, "the sweep made plans");

    if (failures) {
        printf("dsi_plan host tests: %d FAILED\n", failures);
        return 1;
    }
    printf("dsi_plan host logic tests passed (%u swept plans)\n", made);
    return 0;
}
