/* =============================================================================
 * HYDRA-UMC Firmware - tests/test_dac_plan.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Host test of the code of the analog outputs (src/mcu_stm32h757/Common/dac_plan.h): literal codes for several voltages and gains (the same ones as tests/test_dac_plan.py against the
 * Python twin), the rounding to the nearest code, the ceiling of each output, a code that would pass 4095, the refusal of every number out of range with its own code, and a sweep in
 * which the voltage that a code gives back never differs from the one asked for by more than one step. Nothing is written to the output in any refusal. Evidence: simulator.
 *
 *   cc -std=c11 -Wall -Wextra -Isrc/mcu_stm32h757/Common -o test_dac_plan tests/test_dac_plan.c
 */
#include <stdio.h>

#include "dac_plan.h"

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } } while (0)

static DacpConfig config(void)
{
    DacpConfig c = {3300u, 3u, 1u, {9900u, 9900u}};
    return c;
}

int main(void)
{
    DacpConfig c = config();
    uint32_t code = 0x5A5A;

    /* the codes */
    CHECK(DacPlan_CodeFor(&c, 1u, 0u, &code) == DACP_OK && code == 0u, "0 V is code 0");
    CHECK(DacPlan_CodeFor(&c, 1u, 9900u, &code) == DACP_OK && code == 4095u, "9.9 V through a gain of 3 is full scale");
    CHECK(DacPlan_CodeFor(&c, 2u, 4950u, &code) == DACP_OK && code == 2048u, "half of it is 2048 (2047.5 rounded up)");
    CHECK(DacPlan_CodeFor(&c, 1u, 1000u, &code) == DACP_OK && code == 414u, "1 V is 413.6, code 414");
    CHECK(DacPlan_CodeFor(&c, 1u, 1u, &code) == DACP_OK && code == 0u, "1 mV is under half a step");
    CHECK(DacPlan_CodeFor(&c, 1u, 3u, &code) == DACP_OK && code == 1u, "3 mV is one step");
    c.vref_mv = 2500u; c.gain_num = 4u; c.gain_den = 1u; c.ceiling_mv[0] = 10000u;
    CHECK(DacPlan_CodeFor(&c, 1u, 10000u, &code) == DACP_OK && code == 4095u, "10 V with a 2.5 V reference and a gain of 4");
    c.gain_num = 10u; c.gain_den = 3u;
    CHECK(DacPlan_CodeFor(&c, 1u, 5000u, &code) == DACP_OK && code == 2457u, "a gain of 10 to 3 (5 V is 1.5 V at the pin, 2457.0)");

    /* the ceiling and the range */
    c = config();
    code = 0x5A5A;
    CHECK(DacPlan_CodeFor(&c, 1u, 9901u, &code) == DACP_E_RANGE && code == 0x5A5Au, "over the ceiling is refused and nothing is written");
    c.ceiling_mv[1] = 5000u;
    CHECK(DacPlan_CodeFor(&c, 2u, 5001u, &code) == DACP_E_RANGE && DacPlan_CodeFor(&c, 1u, 9900u, &code) == DACP_OK, "each output has its own ceiling");
    c = config(); c.ceiling_mv[0] = 10000u;
    CHECK(DacPlan_CodeFor(&c, 1u, 10000u, &code) == DACP_E_RANGE && code != 4096u, "the ceiling allows 10 V but the converter would need a code over 4095");

    /* the channel and the arguments */
    c = config();
    CHECK(DacPlan_CodeFor(&c, 0u, 1u, &code) == DACP_E_CHANNEL && DacPlan_CodeFor(&c, 3u, 1u, &code) == DACP_E_CHANNEL, "outputs 1 and 2 only");
    CHECK(DacPlan_CodeFor(NULL, 1u, 1u, &code) == DACP_E_ARG && DacPlan_CodeFor(&c, 1u, 1u, NULL) == DACP_E_ARG, "missing arguments");

    /* the configuration */
    CHECK(DacPlan_CheckConfig(&c) == DACP_OK, "the defaults");
    c = config(); c.vref_mv = 1799u;   CHECK(DacPlan_CheckConfig(&c) == DACP_E_VREF, "vref under");
    c = config(); c.vref_mv = 3601u;   CHECK(DacPlan_CheckConfig(&c) == DACP_E_VREF, "vref over");
    c = config(); c.gain_num = 0u;     CHECK(DacPlan_CheckConfig(&c) == DACP_E_GAIN, "numerator 0");
    c = config(); c.gain_den = 0u;     CHECK(DacPlan_CheckConfig(&c) == DACP_E_GAIN, "denominator 0");
    c = config(); c.gain_num = 1001u;  CHECK(DacPlan_CheckConfig(&c) == DACP_E_GAIN, "numerator over");
    c = config(); c.gain_den = 1001u;  CHECK(DacPlan_CheckConfig(&c) == DACP_E_GAIN, "denominator over");
    c = config(); c.ceiling_mv[0] = 10001u; CHECK(DacPlan_CheckConfig(&c) == DACP_E_CEILING, "ceiling of output 1 over 10 V");
    c = config(); c.ceiling_mv[1] = 10001u; CHECK(DacPlan_CheckConfig(&c) == DACP_E_CEILING, "ceiling of output 2 over 10 V");
    CHECK(DacPlan_CheckConfig(NULL) == DACP_E_ARG, "no configuration");
    c = config(); c.vref_mv = 3601u;
    CHECK(DacPlan_CodeFor(&c, 1u, 1u, &code) == DACP_E_VREF, "a bad configuration is reported by the code");

    /* the sweep: the voltage a code gives back is within one step of the one asked for */
    c = config();
    unsigned checked = 0u;
    for (uint32_t mv = 0u; mv <= 9900u; mv += 7u) {
        if (DacPlan_CodeFor(&c, 1u, mv, &code) != DACP_OK) { printf("  FAIL: sweep refused %lu mV\n", (unsigned long)mv); failures++; continue; }
        const double back = (double)code * 3300.0 / 4095.0 * 3.0;
        const double step = 3300.0 / 4095.0 * 3.0;
        if (back > (double)mv + step / 2.0 + 1e-6 || back < (double)mv - step / 2.0 - 1e-6) { printf("  FAIL: sweep %lu mV gave %f\n", (unsigned long)mv, back); failures++; }
        checked++;
    }
    CHECK(checked > 1000u, "the sweep covered the range");

    if (failures) {
        printf("dac_plan host tests: %d FAILED\n", failures);
        return 1;
    }
    printf("dac_plan host logic tests passed (%u voltages checked)\n", checked);
    return 0;
}
