/* =============================================================================
 * HYDRA-UMC Firmware - tests/test_adc_plan.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Host test of the meaning of an ADC reading (src/mcu_stm32h757/Common/adc_plan.h): literal conversions in 12 and 16 bits (the same ones as tests/test_adc_plan.py against the
 * Python twin), the rounding of the average, the offset and the ratio, and the refusal of every number out of range with its own code, a raw value over the resolution and a result
 * that does not fit 32 bits (nothing is written to the output in any refusal). Evidence: simulator.
 *
 *   cc -std=c11 -Wall -Wextra -Isrc/mcu_stm32h757/Common -o test_adc_plan tests/test_adc_plan.c
 */
#include <stdio.h>

#include "adc_plan.h"

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } } while (0)

static AdcpConfig config(void)
{
    AdcpConfig c = {3300u, 16u, 8u, 10u, 1u};
    return c;
}

static AdcpChannel channel(void)
{
    AdcpChannel ch = {1u, 16u, 1u, 1u, 0};
    return ch;
}

static int convert(const AdcpConfig *c, const AdcpChannel *ch, uint32_t raw, int32_t *mv)
{
    return AdcPlan_ToMillivolts(c, ch, raw, mv);
}

int main(void)
{
    AdcpConfig c = config();
    AdcpChannel ch = channel();
    int32_t mv = 0x5A5A;

    /* the conversions */
    CHECK(convert(&c, &ch, 0u, &mv) == ADCP_OK && mv == 0, "zero");
    CHECK(convert(&c, &ch, 65535u, &mv) == ADCP_OK && mv == 3300, "full scale in 16 bits");
    CHECK(convert(&c, &ch, 32768u, &mv) == ADCP_OK && mv == 1650, "half scale in 16 bits");
    c.bits = 12u;
    CHECK(convert(&c, &ch, 4095u, &mv) == ADCP_OK && mv == 3300, "full scale in 12 bits");
    CHECK(convert(&c, &ch, 1024u, &mv) == ADCP_OK && mv == 825, "a quarter in 12 bits");
    c = config();
    ch.num = 11u; ch.den = 1u; /* a divider of 1 to 11 */
    CHECK(convert(&c, &ch, 32768u, &mv) == ADCP_OK && mv == 18150, "a divider of 11 brings 1650 mV at the pin to 18150 mV");
    ch.num = 1u; ch.den = 2u; ch.offset_mv = -250;
    CHECK(convert(&c, &ch, 32768u, &mv) == ADCP_OK && mv == 575, "a ratio of 1 to 2 and an offset");
    ch.num = 1u; ch.den = 3u; ch.offset_mv = 0;
    CHECK(convert(&c, &ch, 65535u, &mv) == ADCP_OK && mv == 1100, "rounding to the nearest: 3300 / 3");
    ch = channel();
    CHECK(convert(&c, &ch, 1u, &mv) == ADCP_OK && mv == 0 && convert(&c, &ch, 20u, &mv) == ADCP_OK && mv == 1, "a count of 1 is under half a millivolt, 20 counts are one");
    ch.offset_mv = -500;
    CHECK(convert(&c, &ch, 0u, &mv) == ADCP_OK && mv == -500, "a negative signal is allowed");

    /* the average */
    CHECK(AdcPlan_Average(80u, 8u) == 10u && AdcPlan_Average(83u, 8u) == 10u && AdcPlan_Average(84u, 8u) == 11u && AdcPlan_Average(7u, 1u) == 7u, "the average is to the nearest");

    /* a result that does not fit */
    ch = channel();
    ch.num = 1000000UL; ch.den = 1u;
    mv = 0x5A5A;
    CHECK(convert(&c, &ch, 65535u, &mv) == ADCP_E_RANGE && mv == 0x5A5A, "3300 mV times a million does not fit");
    c.bits = 12u;
    CHECK(convert(&c, &ch, 4096u, &mv) == ADCP_E_RAW && mv == 0x5A5A, "a raw value over the resolution");

    /* the configuration */
    c = config(); CHECK(AdcPlan_CheckConfig(&c) == ADCP_OK, "the defaults");
    c = config(); c.vref_mv = 1799u; CHECK(AdcPlan_CheckConfig(&c) == ADCP_E_VREF, "vref under");
    c = config(); c.vref_mv = 3601u; CHECK(AdcPlan_CheckConfig(&c) == ADCP_E_VREF, "vref over");
    c = config(); c.bits = 14u;      CHECK(AdcPlan_CheckConfig(&c) == ADCP_E_BITS, "14 bits is not offered");
    c = config(); c.samples = 0u;    CHECK(AdcPlan_CheckConfig(&c) == ADCP_E_SAMPLES, "no samples");
    c = config(); c.samples = 65u;   CHECK(AdcPlan_CheckConfig(&c) == ADCP_E_SAMPLES, "too many samples");
    c = config(); c.timeout_ms = 0u; CHECK(AdcPlan_CheckConfig(&c) == ADCP_E_TIMEOUT, "no time-out");
    c = config(); c.timeout_ms = 1001u; CHECK(AdcPlan_CheckConfig(&c) == ADCP_E_TIMEOUT, "time-out over");
    c = config(); c.sample_code = 3u; CHECK(AdcPlan_CheckConfig(&c) == ADCP_E_SAMPLE_CODE, "sampling time code over");
    CHECK(AdcPlan_CheckConfig(NULL) == ADCP_E_ARG, "no configuration");
    c = config(); c.vref_mv = 1800u; c.samples = 64u; c.timeout_ms = 1000u; c.sample_code = 2u; c.bits = 12u;
    CHECK(AdcPlan_CheckConfig(&c) == ADCP_OK, "the edges that are accepted");

    /* the channel */
    ch = channel(); CHECK(AdcPlan_CheckChannel(&ch) == ADCP_OK, "a good channel");
    ch = channel(); ch.adc = 0u;      CHECK(AdcPlan_CheckChannel(&ch) == ADCP_E_ADC, "an input not yet assigned to an ADC");
    ch = channel(); ch.adc = 4u;      CHECK(AdcPlan_CheckChannel(&ch) == ADCP_E_ADC, "no fourth ADC");
    ch = channel(); ch.channel = 20u; CHECK(AdcPlan_CheckChannel(&ch) == ADCP_E_ADC, "no channel 20");
    ch = channel(); ch.num = 0u;      CHECK(AdcPlan_CheckChannel(&ch) == ADCP_E_RATIO, "a ratio numerator of 0");
    ch = channel(); ch.den = 0u;      CHECK(AdcPlan_CheckChannel(&ch) == ADCP_E_RATIO, "a ratio denominator of 0");
    ch = channel(); ch.num = 1000001UL; CHECK(AdcPlan_CheckChannel(&ch) == ADCP_E_RATIO, "a numerator over a million");
    ch = channel(); ch.den = 1000001UL; CHECK(AdcPlan_CheckChannel(&ch) == ADCP_E_RATIO, "a denominator over a million");
    ch = channel(); ch.offset_mv = 100001; CHECK(AdcPlan_CheckChannel(&ch) == ADCP_E_OFFSET, "offset over");
    ch = channel(); ch.offset_mv = -100001; CHECK(AdcPlan_CheckChannel(&ch) == ADCP_E_OFFSET, "offset under");
    CHECK(AdcPlan_CheckChannel(NULL) == ADCP_E_ARG, "no channel");

    /* arguments of the conversion */
    c = config(); ch = channel();
    CHECK(convert(NULL, &ch, 1u, &mv) == ADCP_E_ARG && convert(&c, NULL, 1u, &mv) == ADCP_E_ARG && convert(&c, &ch, 1u, NULL) == ADCP_E_ARG, "missing arguments");
    c.vref_mv = 0u;
    CHECK(convert(&c, &ch, 1u, &mv) == ADCP_E_VREF, "a bad configuration is reported by the conversion");
    c = config(); ch.den = 0u;
    CHECK(convert(&c, &ch, 1u, &mv) == ADCP_E_RATIO, "a bad channel is reported by the conversion");

    if (failures) {
        printf("adc_plan host tests: %d FAILED\n", failures);
        return 1;
    }
    printf("adc_plan host logic tests passed\n");
    return 0;
}
