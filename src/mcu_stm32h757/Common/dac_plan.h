/* =============================================================================
 * HYDRA-UMC Firmware - src/mcu_stm32h757/Common/dac_plan.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * dac_plan.h - the code of the 12-bit DAC for a voltage on the 0 to 10 V analog outputs of the board (HAL-free, host-testable). The two outputs (AO1 on PA4, AO2 on PA5) are the DAC
 * pins behind an amplifier of which the file gives the gain as a ratio; the voltage asked for is divided by the gain to get the millivolts at the pin and then turned into a code from
 * the reference voltage, to the nearest. The voltage is held to a ceiling that the file sets (at most 10 V): a request over it, a negative one or one that would need a code over 4095
 * is refused, never clipped, so an output cannot be driven higher than the file allows. A channel that is not 1 or 2 is refused. The converter itself is CM7/DacOut.c;
 * tools/dac_plan.py is the Python twin and reads config/dac.cfg. Nothing here has run on a board.
 * ============================================================================= */
#ifndef HYDRA_UMC_DAC_PLAN_H
#define HYDRA_UMC_DAC_PLAN_H

#include <stdint.h>

#define DACP_VREF_MIN_MV 1800UL
#define DACP_VREF_MAX_MV 3600UL
#define DACP_RATIO_MAX 1000UL
#define DACP_OUT_MAX_MV 10000UL
#define DACP_CODE_MAX 4095UL
#define DACP_CHANNELS 2u

#define DACP_OK 0
#define DACP_E_ARG 1
#define DACP_E_VREF 2
#define DACP_E_GAIN 3
#define DACP_E_CEILING 4
#define DACP_E_CHANNEL 5
#define DACP_E_RANGE 6 /* over the ceiling, or a code over 4095 would be needed */

typedef struct {
    uint32_t vref_mv;
    uint32_t gain_num, gain_den;    /* output volts = volts at the pin * gain_num / gain_den */
    uint32_t ceiling_mv[DACP_CHANNELS]; /* the highest voltage each output may be asked for */
} DacpConfig;

static inline int DacPlan_CheckConfig(const DacpConfig *c)
{
    if (!c) return DACP_E_ARG;
    if (c->vref_mv < DACP_VREF_MIN_MV || c->vref_mv > DACP_VREF_MAX_MV) return DACP_E_VREF;
    if (c->gain_num < 1u || c->gain_num > DACP_RATIO_MAX || c->gain_den < 1u || c->gain_den > DACP_RATIO_MAX) return DACP_E_GAIN;
    for (unsigned i = 0u; i < DACP_CHANNELS; i++) {
        if (c->ceiling_mv[i] > DACP_OUT_MAX_MV) return DACP_E_CEILING;
    }
    return DACP_OK;
}

/* The code for `out_mv` on `channel` (1 or 2). `code` is written only on success. */
static inline int DacPlan_CodeFor(const DacpConfig *c, unsigned channel, uint32_t out_mv, uint32_t *code)
{
    int rc = DacPlan_CheckConfig(c);
    if (rc != DACP_OK) return rc;
    if (!code) return DACP_E_ARG;
    if (channel < 1u || channel > DACP_CHANNELS) return DACP_E_CHANNEL;
    if (out_mv > c->ceiling_mv[channel - 1u]) return DACP_E_RANGE;
    const uint64_t pin_mv_num = (uint64_t)out_mv * c->gain_den; /* the millivolts at the pin times gain_num */
    const uint64_t scaled = pin_mv_num * DACP_CODE_MAX;
    const uint64_t divisor = (uint64_t)c->vref_mv * c->gain_num;
    const uint64_t value = (scaled + divisor / 2u) / divisor;
    if (value > DACP_CODE_MAX) return DACP_E_RANGE;
    *code = (uint32_t)value;
    return DACP_OK;
}

#endif /* HYDRA_UMC_DAC_PLAN_H */
