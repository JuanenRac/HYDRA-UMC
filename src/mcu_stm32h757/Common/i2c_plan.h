/* =============================================================================
 * HYDRA-UMC Firmware - src/mcu_stm32h757/Common/i2c_plan.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * i2c_plan.h - the TIMINGR word of an I2C peripheral of the H7 (HAL-free, host-testable). From the kernel clock, the bus speed, the rise and fall times of the lines and the
 * filters it picks the smallest prescaler for which the set-up time of the data (SCLDEL), the hold time (SDADEL) and the low and high times of the clock all fit the I2C
 * specification of the mode (standard up to 100 kHz, fast up to 400 kHz) with the limits of the reference manual, and builds the word. The bus is never faster than asked: the
 * period is rounded up. When no prescaler fits, nothing is made (the caller keeps the peripheral off) instead of a word that could run the bus out of specification. The
 * times are in picoseconds in a 64-bit integer, so there is no rounding of floating point. tools/i2c_plan.py is the Python twin and reads config/i2c4.cfg; the check against the
 * rise and fall times of the real bus is the bench row 31 of docs/BENCH_TESTS.md. Nothing here has run on a board.
 * ============================================================================= */
#ifndef HYDRA_UMC_I2C_PLAN_H
#define HYDRA_UMC_I2C_PLAN_H

#include <stdint.h>

/* starting values for the bench (config/i2c4.cfg): the kernel clock of I2C4 from the clock tree, standard mode, 4.7 kOhm pull-ups on a short bus */
#define I2CP_DEFAULT_KERNEL_HZ 100000000UL
#define I2CP_DEFAULT_SPEED_HZ 100000UL
#define I2CP_DEFAULT_RISE_NS 400UL
#define I2CP_DEFAULT_FALL_NS 100UL
#define I2CP_DEFAULT_ANALOG_FILTER 1UL
#define I2CP_DEFAULT_DIGITAL_FILTER 0UL

#define I2CP_KERNEL_MIN_HZ 1000000UL
#define I2CP_KERNEL_MAX_HZ 200000000UL
#define I2CP_SPEED_MIN_HZ 10000UL
#define I2CP_SPEED_MAX_HZ 400000UL
#define I2CP_STANDARD_MAX_HZ 100000UL
#define I2CP_DIGITAL_FILTER_MAX 15UL

/* the longest rise and fall times of the specification, and the range of the analog filter of the peripheral, in nanoseconds */
#define I2CP_RISE_MAX_STANDARD_NS 1000UL
#define I2CP_RISE_MAX_FAST_NS 300UL
#define I2CP_FALL_MAX_NS 300UL
#define I2CP_AF_MIN_NS 50UL
#define I2CP_AF_MAX_NS 260UL

#define I2CP_OK 0
#define I2CP_E_ARG 1
#define I2CP_E_KERNEL 2
#define I2CP_E_SPEED 3
#define I2CP_E_RISE 4
#define I2CP_E_FALL 5
#define I2CP_E_FILTER 6
#define I2CP_E_TIMING 7 /* no prescaler fits the specification */

typedef struct {
    uint32_t kernel_hz, speed_hz, rise_ns, fall_ns, analog_filter, digital_filter;
} I2cpConfig;

typedef struct {
    uint32_t timingr;
    uint32_t presc, scldel, sdadel, sclh, scll; /* the register fields */
    uint32_t actual_hz;                          /* never above the speed asked for */
} I2cpPlan;

static inline void I2cPlan_Defaults(I2cpConfig *c)
{
    c->kernel_hz = I2CP_DEFAULT_KERNEL_HZ;
    c->speed_hz = I2CP_DEFAULT_SPEED_HZ;
    c->rise_ns = I2CP_DEFAULT_RISE_NS;
    c->fall_ns = I2CP_DEFAULT_FALL_NS;
    c->analog_filter = I2CP_DEFAULT_ANALOG_FILTER;
    c->digital_filter = I2CP_DEFAULT_DIGITAL_FILTER;
}

static inline uint64_t I2cPlan_CeilDiv(uint64_t a, uint64_t b)
{
    return (a + b - 1u) / b;
}

/* Returns I2CP_OK or the first error; `plan` is written only on success. */
static inline int I2cPlan_Make(const I2cpConfig *c, I2cpPlan *plan)
{
    if (!c || !plan) return I2CP_E_ARG;
    if (c->kernel_hz < I2CP_KERNEL_MIN_HZ || c->kernel_hz > I2CP_KERNEL_MAX_HZ) return I2CP_E_KERNEL;
    if (c->speed_hz < I2CP_SPEED_MIN_HZ || c->speed_hz > I2CP_SPEED_MAX_HZ) return I2CP_E_SPEED;
    const int fast = c->speed_hz > I2CP_STANDARD_MAX_HZ;
    if (c->rise_ns < 1u || c->rise_ns > (fast ? I2CP_RISE_MAX_FAST_NS : I2CP_RISE_MAX_STANDARD_NS)) return I2CP_E_RISE;
    if (c->fall_ns < 1u || c->fall_ns > I2CP_FALL_MAX_NS) return I2CP_E_FALL;
    if (c->analog_filter > 1u || c->digital_filter > I2CP_DIGITAL_FILTER_MAX) return I2CP_E_FILTER;

    /* I2C specification of the mode, in ns: the clock low and high times, the set-up time of the data, the longest time the device may take to give the data after the clock falls */
    const uint64_t low_min = fast ? 1300u : 4700u;
    const uint64_t high_min = fast ? 600u : 4000u;
    const uint64_t setup = fast ? 100u : 250u;
    const uint64_t valid_max = fast ? 900u : 3450u;

    const uint64_t clk = 1000000000000ull / c->kernel_hz; /* ps */
    const uint64_t dnf = c->digital_filter;
    const uint64_t af_min = c->analog_filter ? I2CP_AF_MIN_NS : 0u;
    const uint64_t af_max = c->analog_filter ? I2CP_AF_MAX_NS : 0u;
    const uint64_t rise = c->rise_ns, fall = c->fall_ns;

    const uint64_t scldel_min = (rise + setup) * 1000u;
    const uint64_t hold_cost = fall * 1000u > af_min * 1000u + (dnf + 3u) * clk ? fall * 1000u - af_min * 1000u - (dnf + 3u) * clk : 0u; /* the data hold time is zero in the specification */
    const uint64_t fixed_max = (rise + af_max) * 1000u + (dnf + 4u) * clk;
    if (valid_max * 1000u <= fixed_max) return I2CP_E_TIMING;
    const uint64_t sdadel_max = valid_max * 1000u - fixed_max;
    const uint64_t sync = (rise + fall + 2u * af_min) * 1000u + 2u * (dnf + 2u) * clk; /* the lines and the filters, in ps, that add to the period */
    const uint64_t period = I2cPlan_CeilDiv(1000000000000ull, c->speed_hz); /* rounded up: the bus is never faster than asked */
    /* the period is always longer than the sync time: the check on the data valid time above bounds the filters (under 7.8 us in standard mode, 1.4 us in fast mode, against 10 and 2.5 us) */

    for (uint32_t presc = 0u; presc < 16u; presc++) {
        const uint64_t tp = (uint64_t)(presc + 1u) * clk;
        const uint64_t scldel_steps = I2cPlan_CeilDiv(scldel_min, tp); /* tSCLDEL = (SCLDEL + 1) * tp */
        const uint64_t sdadel = I2cPlan_CeilDiv(hold_cost, tp);        /* tSDADEL = SDADEL * tp */
        if (scldel_steps > 16u || sdadel > 15u || sdadel * tp > sdadel_max) continue;
        const uint64_t low = I2cPlan_CeilDiv(low_min * 1000u, tp);
        const uint64_t high = I2cPlan_CeilDiv(high_min * 1000u, tp);
        const uint64_t steps = I2cPlan_CeilDiv(period - sync, tp);
        if (steps < low + high) continue;
        const uint64_t extra = steps - low - high;
        const uint64_t scll = low + (extra - extra / 2u);
        const uint64_t sclh = high + extra / 2u;
        if (scll > 256u || sclh > 256u) continue;
        plan->presc = presc;
        plan->scldel = (uint32_t)scldel_steps - 1u;
        plan->sdadel = (uint32_t)sdadel;
        plan->scll = (uint32_t)scll - 1u;
        plan->sclh = (uint32_t)sclh - 1u;
        plan->timingr = (presc << 28) | (plan->scldel << 20) | (plan->sdadel << 16) | (plan->sclh << 8) | plan->scll;
        plan->actual_hz = (uint32_t)(1000000000000ull / ((scll + sclh) * tp + sync));
        return I2CP_OK;
    }
    return I2CP_E_TIMING;
}

#endif /* HYDRA_UMC_I2C_PLAN_H */
