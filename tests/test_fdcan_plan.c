/* =============================================================================
 * HYDRA-UMC Firmware - tests/test_fdcan_plan.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Host test of the plan of FDCAN1 (src/mcu_stm32h757/Common/fdcan_plan.h): the timing for the clock tree of the H757 board is the one of the platform profile, other kernel clocks
 * and bit rates give an exact timing or are refused, the sample point is rounded to a quantum and segment 2 is never empty, the jump width never passes segment 2, the filters are the
 * two of the CM4, and every number out of range is refused with its own code. The same cases are in tests/test_fdcan_plan.py against the Python twin. Evidence: simulator.
 *
 *   cc -std=c11 -Wall -Wextra -DHYDRA_PLATFORM_H757 -Isrc/mcu_stm32h757/Common -o test_fdcan_plan tests/test_fdcan_plan.c
 */
#include <stdio.h>
#include <string.h>

#include "fdcan_plan.h"
#include "platform_profile.h"

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } } while (0)

static FdcpConfig base(void)
{
    FdcpConfig c;
    FdcanPlan_Defaults(&c);
    return c;
}

static int make(const FdcpConfig *c, FdcpPlan *p)
{
    FdcpPlan scratch;
    return FdcanPlan_Make(c, p ? p : &scratch);
}

int main(void)
{
    FdcpPlan p;
    FdcpConfig c = base();

    /* the starting values are the ones of the platform profile (same prescaler and segments), and the bus runs at exactly 1 Mbit/s */
    CHECK(FDCP_DEFAULT_KERNEL_HZ == HYDRA_STACK_BUS_KERNEL_CLOCK_HZ && FDCP_DEFAULT_BIT_RATE_BPS == HYDRA_STACK_BUS_BIT_RATE_BPS, "the defaults are the profile");
    CHECK(FdcanPlan_Make(&c, &p) == FDCP_OK, "the defaults give a plan");
    CHECK(p.prescaler == HYDRA_STACK_BUS_PRESCALER && p.seg1 == HYDRA_STACK_BUS_TIME_SEG1 && p.seg2 == HYDRA_STACK_BUS_TIME_SEG2, "the timing of the profile");
    CHECK(p.sjw == 3u && HYDRA_STACK_BUS_SYNC_JUMP_WIDTH == 3u && p.sjw <= p.seg2, "the jump width is the one of the profile and not longer than segment 2");
    CHECK(p.actual_bps == 1000000u && p.actual_sample_permille == 850u, "1 Mbit/s with the sample point at 85 percent");
    CHECK(p.rx_elements == 8u && p.tx_elements == 8u, "the FIFOs");
    CHECK(p.filter_count == 2u && p.filter[0].id == 0x600u && p.filter[0].mask == 0x600u && p.filter[1].id == 0x001u && p.filter[1].mask == 0x7FFu, "the two filters of the CM4");

    /* without the emergency stop filter only the window of the boards is taken */
    c.estop_filter = 0u;
    CHECK(FdcanPlan_Make(&c, &p) == FDCP_OK && p.filter_count == 1u && p.filter[0].id == 0x600u, "one filter");

    /* other clocks: 80 MHz at 500 kbit/s (prescaler 8 gives 20 quanta), 48 MHz at 1 Mbit/s (prescaler 3, 16 quanta), 40 MHz at 125 kbit/s */
    c = base(); c.kernel_hz = 80000000UL; c.bit_rate_bps = 500000UL;
    CHECK(FdcanPlan_Make(&c, &p) == FDCP_OK && p.prescaler == 8u && p.seg1 == 16u && p.seg2 == 3u && p.actual_bps == 500000u, "80 MHz at 500 kbit/s");
    c = base(); c.kernel_hz = 48000000UL; c.max_quanta = 16u;
    CHECK(FdcanPlan_Make(&c, &p) == FDCP_OK && p.prescaler == 3u && p.seg1 == 13u && p.seg2 == 2u && p.actual_sample_permille == 875u, "48 MHz at 1 Mbit/s");
    c = base(); c.kernel_hz = 40000000UL; c.bit_rate_bps = 125000UL;
    CHECK(FdcanPlan_Make(&c, &p) == FDCP_OK && p.prescaler == 16u && p.seg1 + p.seg2 + 1u == 20u, "40 MHz at 125 kbit/s");

    /* a prescaler that does not divide exactly is skipped: 50 MHz at 1 Mbit/s with at most 20 quanta is prescaler 3? no (50 / 3 is not whole) - it takes 5 with 10 quanta */
    c = base(); c.kernel_hz = 50000000UL;
    CHECK(FdcanPlan_Make(&c, &p) == FDCP_OK && p.prescaler == 3u + 2u && p.seg1 + p.seg2 + 1u == 10u, "an inexact prescaler is skipped");

    /* the sample point at the top with the shortest bit: segment 2 keeps one quantum */
    c = base(); c.kernel_hz = 8000000UL; c.sample_permille = 950u;
    CHECK(FdcanPlan_Make(&c, &p) == FDCP_OK && p.prescaler == 1u && p.seg1 == 6u && p.seg2 == 1u && p.sjw == 1u, "segment 2 is never empty");

    /* no timing: the clock does not divide into the bit rate, the bit would be shorter than 8 quanta, or too many quanta even with the largest prescaler */
    c = base(); c.kernel_hz = 7000000UL;
    CHECK(make(&c, NULL) == FDCP_E_TIMING, "7 MHz at 1 Mbit/s is under 8 quanta");
    c = base(); c.kernel_hz = 100000007UL;
    CHECK(make(&c, NULL) == FDCP_E_TIMING, "a clock that divides by no prescaler");
    c = base(); c.kernel_hz = 200000000UL; c.bit_rate_bps = 10000UL; c.max_quanta = 8u;
    CHECK(make(&c, NULL) == FDCP_E_TIMING, "the prescaler runs out");

    /* every number out of range has its own code, and a plan is written only on success */
    memset(&p, 0x5A, sizeof p);
    c = base(); c.kernel_hz = 999999UL;       CHECK(make(&c, &p) == FDCP_E_KERNEL, "kernel clock under");
    c = base(); c.kernel_hz = 200000001UL;    CHECK(make(&c, &p) == FDCP_E_KERNEL, "kernel clock over");
    c = base(); c.bit_rate_bps = 9999UL;      CHECK(make(&c, &p) == FDCP_E_BIT_RATE, "bit rate under");
    c = base(); c.bit_rate_bps = 1000001UL;   CHECK(make(&c, &p) == FDCP_E_BIT_RATE, "bit rate over");
    c = base(); c.sample_permille = 499u;     CHECK(make(&c, &p) == FDCP_E_SAMPLE, "sample point under");
    c = base(); c.sample_permille = 951u;     CHECK(make(&c, &p) == FDCP_E_SAMPLE, "sample point over");
    c = base(); c.max_quanta = 7u;            CHECK(make(&c, &p) == FDCP_E_QUANTA, "quanta under");
    c = base(); c.max_quanta = 129u;          CHECK(make(&c, &p) == FDCP_E_QUANTA, "quanta over");
    c = base(); c.sjw = 0u;                   CHECK(make(&c, &p) == FDCP_E_SJW, "jump width under");
    c = base(); c.sjw = 5u;                   CHECK(make(&c, &p) == FDCP_E_SJW, "jump width over");
    c = base(); c.rx_elements = 0u;           CHECK(make(&c, &p) == FDCP_E_ELEMENTS, "no receive elements");
    c = base(); c.rx_elements = 33u;          CHECK(make(&c, &p) == FDCP_E_ELEMENTS, "too many receive elements");
    c = base(); c.tx_elements = 0u;           CHECK(make(&c, &p) == FDCP_E_ELEMENTS, "no transmit elements");
    c = base(); c.tx_elements = 33u;          CHECK(make(&c, &p) == FDCP_E_ELEMENTS, "too many transmit elements");
    CHECK(p.prescaler == 0x5A5A5A5Au, "nothing was written by a refused plan");
    c = base();
    CHECK(FdcanPlan_Make(NULL, &p) == FDCP_E_ARG && FdcanPlan_Make(&c, NULL) == FDCP_E_ARG, "missing arguments");

    /* the edges that are accepted */
    c = base(); c.kernel_hz = 1000000UL; c.bit_rate_bps = 10000UL; c.max_quanta = 100u; c.sjw = 1u; c.rx_elements = 32u; c.tx_elements = 1u; c.sample_permille = 500u;
    CHECK(FdcanPlan_Make(&c, &p) == FDCP_OK && p.prescaler == 1u && p.seg1 + p.seg2 + 1u == 100u && p.rx_elements == 32u && p.tx_elements == 1u, "the lowest edges");

    /* the second controller (FDCAN2, the field bus) is placed after the first in the shared message RAM */
    {
        FdcpPlan first, field, again;
        c = base();
        CHECK(FdcanPlan_Make(&c, &first) == FDCP_OK && first.ram_offset == 0u && FdcanPlan_RamWords(&first) == 2u + (8u + 8u) * 4u, "the first controller starts at 0 and uses 66 words");
        CHECK(FdcanPlan_MakeField(&c, 0x000u, 0x000u, &first, &field) == FDCP_OK && field.ram_offset == 66u && field.filter_count == 1u && field.filter[0].mask == 0u, "FDCAN2 follows at word 66 with one filter that takes every id");
        CHECK(field.prescaler == first.prescaler && field.seg1 == first.seg1 && field.seg2 == first.seg2, "the same 1 Mbit/s timing");
        CHECK(FdcanPlan_MakeField(&c, 0x123u, 0x7F0u, &field, &again) == FDCP_OK && again.ram_offset == 66u + 1u + 64u && again.filter[0].id == 0x123u, "a third block would follow the second");
        CHECK(FdcanPlan_MakeField(&c, 0x800u, 0u, &first, &field) == FDCP_E_FILTER && FdcanPlan_MakeField(&c, 0u, 0x800u, &first, &field) == FDCP_E_FILTER, "a filter over 11 bits");
        CHECK(FdcanPlan_MakeField(NULL, 0u, 0u, &first, &field) == FDCP_E_ARG && FdcanPlan_MakeField(&c, 0u, 0u, NULL, &field) == FDCP_E_ARG && FdcanPlan_MakeField(&c, 0u, 0u, &first, NULL) == FDCP_E_ARG, "missing arguments");
        c.kernel_hz = 7000000UL;
        CHECK(FdcanPlan_MakeField(&c, 0u, 0u, &first, &field) == FDCP_E_TIMING, "the timing errors pass through");
        c = base();
        first.ram_offset = 2500u; /* a first block that leaves no room */
        memset(&field, 0x5A, sizeof field);
        CHECK(FdcanPlan_MakeField(&c, 0u, 0u, &first, &field) == FDCP_E_RAM && field.ram_offset == 0x5A5A5A5Au, "no room in the message RAM: nothing is written");
        CHECK(FdcanPlan_PlaceAfter(NULL, &field) == FDCP_E_ARG && FdcanPlan_PlaceAfter(&first, NULL) == FDCP_E_ARG, "placing needs both");
    }

    if (failures) {
        printf("fdcan_plan host tests: %d FAILED\n", failures);
        return 1;
    }
    printf("fdcan_plan host logic tests passed\n");
    return 0;
}
