/* =============================================================================
 * HYDRA-UMC Firmware - src/mcu_stm32h757/Common/adc_plan.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * adc_plan.h - what a reading of the ADC means (HAL-free, host-testable). A channel of the board is an input of one of the three ADCs behind a divider (or a current sense) of which the
 * file gives the ratio and an offset; the reading is the average of a number of conversions, turned into millivolts at the pin from the reference voltage and the resolution, then
 * scaled back to the millivolts of the signal: signal = pin * num / den + offset, all in integers with rounding to the nearest. Every number is checked: an ADC that does not exist,
 * a channel over 19, a ratio of 0, a raw value that the resolution cannot give, and a result that does not fit a signed 32-bit number are refused instead of clipped. The ADC itself
 * is CM7/AdcReader.c; tools/adc_plan.py is the Python twin and reads config/adc.cfg. Nothing here has run on a board.
 * ============================================================================= */
#ifndef HYDRA_UMC_ADC_PLAN_H
#define HYDRA_UMC_ADC_PLAN_H

#include <stdint.h>

#define ADCP_VREF_MIN_MV 1800UL
#define ADCP_VREF_MAX_MV 3600UL
#define ADCP_SAMPLES_MIN 1UL
#define ADCP_SAMPLES_MAX 64UL
#define ADCP_TIMEOUT_MIN_MS 1UL
#define ADCP_TIMEOUT_MAX_MS 1000UL
#define ADCP_ADC_MAX 3u
#define ADCP_CHANNEL_MAX 19u
#define ADCP_RATIO_MAX 1000000UL
#define ADCP_OFFSET_MAX_MV 100000L

/* the sampling times the driver offers, in ADC clock cycles: code 0 = 16.5, 1 = 64.5, 2 = 387.5 */
#define ADCP_SAMPLE_CODE_MAX 2UL

#define ADCP_OK 0
#define ADCP_E_ARG 1
#define ADCP_E_VREF 2
#define ADCP_E_BITS 3
#define ADCP_E_SAMPLES 4
#define ADCP_E_TIMEOUT 5
#define ADCP_E_SAMPLE_CODE 6
#define ADCP_E_ADC 7      /* the ADC is not 1, 2 or 3, or the channel is over 19 */
#define ADCP_E_RATIO 8
#define ADCP_E_OFFSET 9
#define ADCP_E_RAW 10     /* the raw value is more than the resolution can give */
#define ADCP_E_RANGE 11   /* the result does not fit a signed 32-bit number */

typedef struct {
    uint32_t vref_mv;
    uint32_t bits; /* 12 or 16 */
    uint32_t samples;
    uint32_t timeout_ms;
    uint32_t sample_code; /* 0, 1 or 2 */
} AdcpConfig;

typedef struct {
    uint8_t adc;     /* 1..3; 0 = the input is not yet assigned to an ADC and cannot be read */
    uint8_t channel; /* 0..19 */
    uint32_t num, den;
    int32_t offset_mv;
} AdcpChannel;

static inline int AdcPlan_CheckConfig(const AdcpConfig *c)
{
    if (!c) return ADCP_E_ARG;
    if (c->vref_mv < ADCP_VREF_MIN_MV || c->vref_mv > ADCP_VREF_MAX_MV) return ADCP_E_VREF;
    if (c->bits != 12u && c->bits != 16u) return ADCP_E_BITS;
    if (c->samples < ADCP_SAMPLES_MIN || c->samples > ADCP_SAMPLES_MAX) return ADCP_E_SAMPLES;
    if (c->timeout_ms < ADCP_TIMEOUT_MIN_MS || c->timeout_ms > ADCP_TIMEOUT_MAX_MS) return ADCP_E_TIMEOUT;
    if (c->sample_code > ADCP_SAMPLE_CODE_MAX) return ADCP_E_SAMPLE_CODE;
    return ADCP_OK;
}

static inline int AdcPlan_CheckChannel(const AdcpChannel *ch)
{
    if (!ch) return ADCP_E_ARG;
    if (ch->adc < 1u || ch->adc > ADCP_ADC_MAX || ch->channel > ADCP_CHANNEL_MAX) return ADCP_E_ADC;
    if (ch->num < 1u || ch->num > ADCP_RATIO_MAX || ch->den < 1u || ch->den > ADCP_RATIO_MAX) return ADCP_E_RATIO;
    if (ch->offset_mv < -ADCP_OFFSET_MAX_MV || ch->offset_mv > ADCP_OFFSET_MAX_MV) return ADCP_E_OFFSET;
    return ADCP_OK;
}

/* The average of `count` conversions that add up to `sum`, to the nearest. */
static inline uint32_t AdcPlan_Average(uint32_t sum, uint32_t count)
{
    return (sum + count / 2u) / count;
}

/* The signal in millivolts for the average raw value of a channel. `mv` is written only on success. */
static inline int AdcPlan_ToMillivolts(const AdcpConfig *c, const AdcpChannel *ch, uint32_t raw, int32_t *mv)
{
    int rc = AdcPlan_CheckConfig(c);
    if (rc != ADCP_OK) return rc;
    rc = AdcPlan_CheckChannel(ch);
    if (rc != ADCP_OK) return rc;
    if (!mv) return ADCP_E_ARG;
    const uint64_t full = (1ull << c->bits) - 1u;
    if (raw > full) return ADCP_E_RAW;
    const uint64_t pin_mv = ((uint64_t)raw * c->vref_mv + full / 2u) / full;
    const int64_t signal = (int64_t)((pin_mv * ch->num + ch->den / 2u) / ch->den) + ch->offset_mv;
    if (signal > INT32_MAX) return ADCP_E_RANGE; /* the offset is at most 100 V, so the result cannot go under the lowest signed number */
    *mv = (int32_t)signal;
    return ADCP_OK;
}

#endif /* HYDRA_UMC_ADC_PLAN_H */
