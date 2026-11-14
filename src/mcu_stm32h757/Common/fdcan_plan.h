/* =============================================================================
 * HYDRA-UMC Firmware - src/mcu_stm32h757/Common/fdcan_plan.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * fdcan_plan.h - the numbers that FDCAN1 of the CM7 needs to be started on the stack bus (HAL-free, host-testable). From the kernel clock, the bit rate, the sample point and the
 * most time quanta a bit may have, it works out the prescaler and the three segments of the bit (the exact bit rate is required: a kernel clock that does not divide into it is
 * refused, not rounded), the sizes of the two FIFOs and the acceptance filters: the whole window of the controller boards (0x600 to 0x7FF) into RX FIFO0 and, when asked for, the
 * emergency stop id, the same two filters as the CM4 of the H745 board (CM4/boot/bootloader_main.c). The driver that writes the numbers into the peripheral is
 * CM7/Fdcan1Start.c; tools/fdcan_plan.py is the Python twin and reads config/fdcan.cfg. Nothing here has run on a board.
 * ============================================================================= */
#ifndef HYDRA_UMC_FDCAN_PLAN_H
#define HYDRA_UMC_FDCAN_PLAN_H

#include <stdint.h>

/* starting values for the bench: the kernel clock of the clock tree of the H757 board (PLL2_Q), the 1 Mbit/s of the stack bus, a sample point at 85 percent (config/fdcan.cfg) */
#define FDCP_DEFAULT_KERNEL_HZ 100000000UL
#define FDCP_DEFAULT_BIT_RATE_BPS 1000000UL
#define FDCP_DEFAULT_SAMPLE_PERMILLE 850UL
#define FDCP_DEFAULT_MAX_QUANTA 20UL
#define FDCP_DEFAULT_SJW 4UL
#define FDCP_DEFAULT_RX_ELEMENTS 8UL
#define FDCP_DEFAULT_TX_ELEMENTS 8UL
#define FDCP_DEFAULT_ESTOP_FILTER 1UL

#define FDCP_KERNEL_MIN_HZ 1000000UL
#define FDCP_KERNEL_MAX_HZ 200000000UL
#define FDCP_BIT_RATE_MIN_BPS 10000UL
#define FDCP_BIT_RATE_MAX_BPS 1000000UL /* Classic CAN */
#define FDCP_SAMPLE_MIN_PERMILLE 500UL
#define FDCP_SAMPLE_MAX_PERMILLE 950UL
#define FDCP_QUANTA_MIN 8UL
#define FDCP_QUANTA_MAX 128UL
#define FDCP_SJW_MIN 1UL
#define FDCP_SJW_MAX 4UL
#define FDCP_ELEMENTS_MIN 1UL
#define FDCP_ELEMENTS_MAX 32UL
#define FDCP_PRESCALER_MAX 512UL

/* the filters of the CM4 of the H745 board: the window 0x600..0x7FF of the controller boards (id 0x600, mask 0x600) and the emergency stop (id 0x001, exact) */
#define FDCP_STACKA_FILTER_ID 0x600UL
#define FDCP_STACKA_FILTER_MASK 0x600UL
#define FDCP_ESTOP_ID 0x001UL
#define FDCP_ESTOP_MASK 0x7FFUL

#define FDCP_OK 0
#define FDCP_E_ARG 1
#define FDCP_E_KERNEL 2
#define FDCP_E_BIT_RATE 3
#define FDCP_E_SAMPLE 4
#define FDCP_E_QUANTA 5
#define FDCP_E_SJW 6
#define FDCP_E_ELEMENTS 7
#define FDCP_E_TIMING 8 /* no prescaler gives exactly the bit rate with a bit of 8..max_quanta time quanta */
#define FDCP_E_FILTER 9 /* the id or the mask of the filter of the field bus is over 11 bits */
#define FDCP_E_RAM 10   /* the second controller does not fit in the message RAM that the two share */

/* the message RAM of the two controllers is one block of 2560 words; a controller uses its standard filters (1 word each), and for each element of a FIFO 4 words (2 of header and 2 of an 8-byte payload) */
#define FDCP_RAM_WORDS_TOTAL 2560UL
#define FDCP_ELEMENT_WORDS 4UL

typedef struct {
    uint32_t kernel_hz, bit_rate_bps, sample_permille, max_quanta, sjw, rx_elements, tx_elements, estop_filter;
} FdcpConfig;

typedef struct {
    uint32_t id, mask;
} FdcpFilter;

typedef struct {
    uint32_t prescaler, sjw, seg1, seg2; /* the values the HAL takes: 1-based, as in Init.NominalPrescaler and the three segments */
    uint32_t actual_bps, actual_sample_permille;
    uint32_t rx_elements, tx_elements;
    uint32_t ram_offset;   /* where this controller starts in the message RAM, in words (Init.MessageRAMOffset) */
    uint32_t filter_count; /* 1 or 2 */
    FdcpFilter filter[2];
} FdcpPlan;

static inline void FdcanPlan_Defaults(FdcpConfig *c)
{
    c->kernel_hz = FDCP_DEFAULT_KERNEL_HZ;
    c->bit_rate_bps = FDCP_DEFAULT_BIT_RATE_BPS;
    c->sample_permille = FDCP_DEFAULT_SAMPLE_PERMILLE;
    c->max_quanta = FDCP_DEFAULT_MAX_QUANTA;
    c->sjw = FDCP_DEFAULT_SJW;
    c->rx_elements = FDCP_DEFAULT_RX_ELEMENTS;
    c->tx_elements = FDCP_DEFAULT_TX_ELEMENTS;
    c->estop_filter = FDCP_DEFAULT_ESTOP_FILTER;
}

static inline int FdcanPlan_InRange(uint32_t v, uint32_t lo, uint32_t hi)
{
    return v >= lo && v <= hi;
}

/* The smallest prescaler whose bit has at most max_quanta time quanta (and at least 8) and gives exactly the bit rate. The sample point is the end of segment 1 rounded to the
 * nearest quantum, with segment 2 never shorter than one quantum; the jump width is the one asked for but never longer than segment 2 (the rule of CAN). Returns FDCP_OK or the
 * first error; `plan` is written only on success. */
static inline int FdcanPlan_Make(const FdcpConfig *c, FdcpPlan *plan)
{
    if (!c || !plan) return FDCP_E_ARG;
    if (!FdcanPlan_InRange(c->kernel_hz, FDCP_KERNEL_MIN_HZ, FDCP_KERNEL_MAX_HZ)) return FDCP_E_KERNEL;
    if (!FdcanPlan_InRange(c->bit_rate_bps, FDCP_BIT_RATE_MIN_BPS, FDCP_BIT_RATE_MAX_BPS)) return FDCP_E_BIT_RATE;
    if (!FdcanPlan_InRange(c->sample_permille, FDCP_SAMPLE_MIN_PERMILLE, FDCP_SAMPLE_MAX_PERMILLE)) return FDCP_E_SAMPLE;
    if (!FdcanPlan_InRange(c->max_quanta, FDCP_QUANTA_MIN, FDCP_QUANTA_MAX)) return FDCP_E_QUANTA;
    if (!FdcanPlan_InRange(c->sjw, FDCP_SJW_MIN, FDCP_SJW_MAX)) return FDCP_E_SJW;
    if (!FdcanPlan_InRange(c->rx_elements, FDCP_ELEMENTS_MIN, FDCP_ELEMENTS_MAX) || !FdcanPlan_InRange(c->tx_elements, FDCP_ELEMENTS_MIN, FDCP_ELEMENTS_MAX)) return FDCP_E_ELEMENTS;

    for (uint32_t prescaler = 1u; prescaler <= FDCP_PRESCALER_MAX; prescaler++) {
        const uint32_t per_quantum = prescaler * c->bit_rate_bps;
        const uint32_t quanta = c->kernel_hz / per_quantum;
        if (quanta < FDCP_QUANTA_MIN) break; /* a larger prescaler only makes the bit shorter */
        if (quanta > c->max_quanta || c->kernel_hz % per_quantum != 0u) continue;
        uint32_t seg1 = (quanta * c->sample_permille + 500u) / 1000u - 1u;
        uint32_t seg2 = quanta - 1u - seg1;
        if (seg2 < 1u) {
            seg2 = 1u;
            seg1 = quanta - 2u;
        }
        plan->prescaler = prescaler;
        plan->seg1 = seg1;
        plan->seg2 = seg2;
        plan->sjw = c->sjw < seg2 ? c->sjw : seg2;
        plan->actual_bps = c->kernel_hz / (prescaler * quanta);
        plan->actual_sample_permille = (1u + seg1) * 1000u / quanta;
        plan->rx_elements = c->rx_elements;
        plan->tx_elements = c->tx_elements;
        plan->filter[0].id = FDCP_STACKA_FILTER_ID;
        plan->filter[0].mask = FDCP_STACKA_FILTER_MASK;
        plan->ram_offset = 0u;
        plan->filter_count = 1u;
        if (c->estop_filter) {
            plan->filter[1].id = FDCP_ESTOP_ID;
            plan->filter[1].mask = FDCP_ESTOP_MASK;
            plan->filter_count = 2u;
        }
        return FDCP_OK;
    }
    return FDCP_E_TIMING;
}

/* The words of the message RAM that a plan uses. */
static inline uint32_t FdcanPlan_RamWords(const FdcpPlan *p)
{
    return p->filter_count + (p->rx_elements + p->tx_elements) * FDCP_ELEMENT_WORDS;
}

/* Puts `second` in the message RAM right after `first`; FDCP_E_RAM (and `second` unchanged) when it would pass the end of the block. */
static inline int FdcanPlan_PlaceAfter(const FdcpPlan *first, FdcpPlan *second)
{
    if (!first || !second) return FDCP_E_ARG;
    const uint32_t offset = first->ram_offset + FdcanPlan_RamWords(first);
    if (offset + FdcanPlan_RamWords(second) > FDCP_RAM_WORDS_TOTAL) return FDCP_E_RAM;
    second->ram_offset = offset;
    return FDCP_OK;
}

/* The plan of the second controller, FDCAN2 of the field bus (CAN_FIELD): the same timing rules and one acceptance filter, id and mask given by the caller (a mask of 0 takes every
 * standard id), placed after `first` in the message RAM. */
static inline int FdcanPlan_MakeField(const FdcpConfig *c, uint32_t id, uint32_t mask, const FdcpPlan *first, FdcpPlan *out)
{
    FdcpPlan made;
    if (!out) return FDCP_E_ARG;
    if (id > 0x7FFUL || mask > 0x7FFUL) return FDCP_E_FILTER;
    int rc = FdcanPlan_Make(c, &made);
    if (rc != FDCP_OK) return rc;
    made.filter[0].id = id;
    made.filter[0].mask = mask;
    made.filter_count = 1u;
    rc = FdcanPlan_PlaceAfter(first, &made);
    if (rc != FDCP_OK) return rc;
    *out = made;
    return FDCP_OK;
}

#endif /* HYDRA_UMC_FDCAN_PLAN_H */
