/* =============================================================================
 * HYDRA-UMC Firmware - tests/test_i2c_plan.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Host test of the timing word of an I2C peripheral (src/mcu_stm32h757/Common/i2c_plan.h): literal words for six cases (the same ones as tests/test_i2c_plan.py against the Python
 * twin), the refusal of every number out of range with its own code, the cases that have no timing, and a sweep over clocks, speeds and line times in which every word that is made
 * is taken apart again and checked against the I2C specification with floating point (an independent check of the integer arithmetic): set-up, hold, low and high times, the data
 * valid time, and a bus that is never faster than asked. Evidence: simulator.
 *
 *   cc -std=c11 -Wall -Wextra -Isrc/mcu_stm32h757/Common -o test_i2c_plan tests/test_i2c_plan.c
 */
#include <stdio.h>
#include <string.h>

#include "i2c_plan.h"

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } } while (0)

static I2cpConfig base(void)
{
    I2cpConfig c;
    I2cPlan_Defaults(&c);
    return c;
}

static int sweep_checks(const I2cpConfig *c, const I2cpPlan *p)
{
    const double clk = 1e9 / (double)c->kernel_hz; /* ns */
    const double tp = (double)(p->presc + 1u) * clk;
    const int fast = c->speed_hz > 100000UL;
    const double low_min = fast ? 1300.0 : 4700.0, high_min = fast ? 600.0 : 4000.0, setup = fast ? 100.0 : 250.0, valid_max = fast ? 900.0 : 3450.0;
    const double af_min = c->analog_filter ? 50.0 : 0.0, af_max = c->analog_filter ? 260.0 : 0.0, dnf = (double)c->digital_filter;
    const double scldel = (double)(p->scldel + 1u) * tp, sdadel = (double)p->sdadel * tp, low = (double)(p->scll + 1u) * tp, high = (double)(p->sclh + 1u) * tp;
    int ok = 1;
    ok &= scldel >= (double)c->rise_ns + setup - 1e-6;                                   /* data set-up before the clock rises */
    ok &= sdadel >= (double)c->fall_ns - af_min - (dnf + 3.0) * clk - 1e-6;             /* the hold time of the data */
    ok &= sdadel <= valid_max - (double)c->rise_ns - af_max - (dnf + 4.0) * clk + 1e-6; /* the data are valid in time */
    ok &= low >= low_min - 1e-6 && high >= high_min - 1e-6;
    ok &= p->actual_hz <= c->speed_hz && p->actual_hz * 100ull >= (unsigned long long)c->speed_hz * 75ull; /* not faster than asked, and within a quarter of it (a low kernel clock has few steps) */
    ok &= p->presc < 16u && p->scldel < 16u && p->sdadel < 16u && p->sclh < 256u && p->scll < 256u;
    ok &= p->timingr == ((p->presc << 28) | (p->scldel << 20) | (p->sdadel << 16) | (p->sclh << 8) | p->scll);
    return ok;
}

int main(void)
{
    I2cpPlan p;
    I2cpConfig c = base();

    /* the words */
    CHECK(I2cPlan_Make(&c, &p) == I2CP_OK && p.timingr == 0x40C15664UL && p.presc == 4u && p.scldel == 12u && p.sdadel == 1u && p.sclh == 86u && p.scll == 100u && p.actual_hz == 99601u, "the defaults");
    c = base(); c.speed_hz = 400000UL; c.rise_ns = 200u; c.fall_ns = 50u;
    CHECK(I2cPlan_Make(&c, &p) == I2CP_OK && p.timingr == 0x10E02246UL && p.actual_hz == 398406u, "fast mode");
    c = base(); c.kernel_hz = 8000000UL;
    CHECK(I2cPlan_Make(&c, &p) == I2CP_OK && p.timingr == 0x00502026UL && p.presc == 0u, "8 MHz");
    c = base(); c.kernel_hz = 48000000UL; c.speed_hz = 400000UL; c.rise_ns = 100u;
    CHECK(I2cPlan_Make(&c, &p) == I2CP_OK && p.timingr == 0x00902143UL, "48 MHz fast");
    c = base(); c.kernel_hz = 120000000UL; c.speed_hz = 300000UL; c.rise_ns = 150u; c.digital_filter = 9u;
    CHECK(I2cPlan_Make(&c, &p) == I2CP_OK && p.timingr == 0x10F03E69UL, "a digital filter");
    c = base(); c.kernel_hz = 200000000UL; c.analog_filter = 0u; c.digital_filter = 15u;
    CHECK(I2cPlan_Make(&c, &p) == I2CP_OK && p.timingr == 0x80E15F6FUL, "no analog filter, the longest digital one");

    /* numbers out of range, one code each, and nothing is written */
    memset(&p, 0x5A, sizeof p);
    c = base(); c.kernel_hz = 999999UL;     CHECK(I2cPlan_Make(&c, &p) == I2CP_E_KERNEL, "kernel under");
    c = base(); c.kernel_hz = 200000001UL;  CHECK(I2cPlan_Make(&c, &p) == I2CP_E_KERNEL, "kernel over");
    c = base(); c.speed_hz = 9999UL;        CHECK(I2cPlan_Make(&c, &p) == I2CP_E_SPEED, "speed under");
    c = base(); c.speed_hz = 400001UL;      CHECK(I2cPlan_Make(&c, &p) == I2CP_E_SPEED, "speed over");
    c = base(); c.rise_ns = 0u;             CHECK(I2cPlan_Make(&c, &p) == I2CP_E_RISE, "no rise time");
    c = base(); c.rise_ns = 1001u;          CHECK(I2cPlan_Make(&c, &p) == I2CP_E_RISE, "rise over in standard mode");
    c = base(); c.speed_hz = 100001UL; c.rise_ns = 301u; CHECK(I2cPlan_Make(&c, &p) == I2CP_E_RISE, "rise over in fast mode");
    c = base(); c.fall_ns = 0u;             CHECK(I2cPlan_Make(&c, &p) == I2CP_E_FALL, "no fall time");
    c = base(); c.fall_ns = 301u;           CHECK(I2cPlan_Make(&c, &p) == I2CP_E_FALL, "fall over");
    c = base(); c.analog_filter = 2u;       CHECK(I2cPlan_Make(&c, &p) == I2CP_E_FILTER, "analog filter 2");
    c = base(); c.digital_filter = 16u;     CHECK(I2cPlan_Make(&c, &p) == I2CP_E_FILTER, "digital filter 16");
    CHECK(p.timingr == 0x5A5A5A5AUL, "nothing was written by a refused plan");
    c = base();
    CHECK(I2cPlan_Make(NULL, &p) == I2CP_E_ARG && I2cPlan_Make(&c, NULL) == I2CP_E_ARG, "missing arguments");

    /* no timing */
    c = base(); c.kernel_hz = 1000000UL;
    CHECK(I2cPlan_Make(&c, &p) == I2CP_E_TIMING, "1 MHz: the filters alone are longer than the data valid time");
    c = base(); c.kernel_hz = 200000000UL; c.speed_hz = 10000UL;
    CHECK(I2cPlan_Make(&c, &p) == I2CP_E_TIMING, "200 MHz at 10 kHz: the low time does not fit 256 steps at the largest prescaler");

    /* the sweep: every word that is made keeps the specification */
    static const uint32_t clocks[] = {2000000UL, 4000000UL, 8000000UL, 16000000UL, 25000000UL, 48000000UL, 64000000UL, 80000000UL, 100000000UL, 120000000UL, 200000000UL};
    static const uint32_t speeds[] = {10000UL, 50000UL, 100000UL, 100001UL, 250000UL, 400000UL};
    static const uint32_t rises[] = {50u, 100u, 300u, 600u};
    unsigned made = 0u, refused = 0u;
    for (unsigned a = 0; a < sizeof clocks / sizeof clocks[0]; a++)
        for (unsigned b = 0; b < sizeof speeds / sizeof speeds[0]; b++)
            for (unsigned r = 0; r < sizeof rises / sizeof rises[0]; r++)
                for (unsigned f = 0; f < 2u; f++)
                    for (unsigned d = 0; d < 16u; d += 5u) {
                        c = base();
                        c.kernel_hz = clocks[a]; c.speed_hz = speeds[b]; c.rise_ns = rises[r]; c.fall_ns = 100u; c.analog_filter = f; c.digital_filter = d;
                        const int rc = I2cPlan_Make(&c, &p);
                        if (rc == I2CP_OK) {
                            made++;
                            if (!sweep_checks(&c, &p)) {
                                printf("  FAIL: sweep %lu Hz %lu Hz rise %lu af %u dnf %u\n", (unsigned long)c.kernel_hz, (unsigned long)c.speed_hz, (unsigned long)c.rise_ns, f, d);
                                failures++;
                            }
                        } else if (rc == I2CP_E_TIMING || rc == I2CP_E_RISE) {
                            refused++;
                        } else {
                            printf("  FAIL: sweep gave code %d\n", rc);
                            failures++;
                        }
                    }
    CHECK(made > 100u && refused > 20u, "the sweep makes words and refuses some");

    if (failures) {
        printf("i2c_plan host tests: %d FAILED\n", failures);
        return 1;
    }
    printf("i2c_plan host logic tests passed (%u words checked, %u refused)\n", made, refused);
    return 0;
}
