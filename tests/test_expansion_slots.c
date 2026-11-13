/* =============================================================================
 * HYDRA-UMC Firmware - tests/test_expansion_slots.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Host test of the power sequence of the expansion slots (src/mcu_stm32h757/Common/expansion_slots.h) against the vectors written from the Python reference
 * (tests/expansion_slots_vectors.h): the state of the three slots, the enable and reset masks and the fault counts must be the same after every update, plus the refusal of
 * configurations that cannot work and the rule that nothing is powered at start. Evidence: simulator.
 *
 *   cc -std=c11 -Wall -Wextra -Itests -Isrc/mcu_stm32h757/Common -o test_expansion_slots tests/test_expansion_slots.c
 */
#include <stdio.h>
#include <string.h>

#include "expansion_slots.h"
#include "expansion_slots_vectors.h"

#define CMD_CLEAR 1
#define CMD_UNLOCK 2

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  FAIL: " __VA_ARGS__); printf("\n"); failures++; } } while (0)

int main(void)
{
    for (unsigned s = 0; s < ES_SCENARIO_COUNT; s++) {
        const es_scenario_t *sc = &es_scenarios[s];
        const es_cfg_t *c = &es_configs[sc->cfg];
        const ExpSlotsConfig cfg = {c->allowed_mask, c->fault_blank_ms, c->power_good_ms, c->reset_release_ms, c->fault_debounce_ms, c->max_faults};
        ExpSlots_t slots;
        if (!ExpSlots_Init(&slots, &cfg)) {
            printf("  FAIL: %s: the configuration was refused\n", sc->name);
            failures++;
            continue;
        }
        ExpSlots_Start(&slots, sc->in[0].t);
        for (unsigned i = 0; i < sc->count; i++) {
            const es_in_t *in = &sc->in[i];
            const es_out_t *want = &sc->out[i];
            if (in->cmd == CMD_CLEAR) ExpSlots_ClearFault(&slots, in->slot);
            if (in->cmd == CMD_UNLOCK) ExpSlots_Unlock(&slots, in->slot, in->t);
            ExpSlots_Update(&slots, in->t, in->present, in->fault);
            if (slots.slot[0].state != want->state[0] || slots.slot[1].state != want->state[1] || slots.slot[2].state != want->state[2] ||
                ExpSlots_EnableMask(&slots) != want->enable || ExpSlots_ResetReleasedMask(&slots) != want->reset || slots.slot[0].faults != want->faults[0] ||
                slots.slot[1].faults != want->faults[1] || slots.slot[2].faults != want->faults[2]) {
                printf("  FAIL: %s step %u: states %u %u %u enable %u reset %u faults %u %u %u, expected %u %u %u %u %u %u %u %u\n", sc->name, i, (unsigned)slots.slot[0].state,
                       (unsigned)slots.slot[1].state, (unsigned)slots.slot[2].state, (unsigned)ExpSlots_EnableMask(&slots), (unsigned)ExpSlots_ResetReleasedMask(&slots),
                       (unsigned)slots.slot[0].faults, (unsigned)slots.slot[1].faults, (unsigned)slots.slot[2].faults, (unsigned)want->state[0], (unsigned)want->state[1],
                       (unsigned)want->state[2], (unsigned)want->enable, (unsigned)want->reset, (unsigned)want->faults[0], (unsigned)want->faults[1], (unsigned)want->faults[2]);
                failures++;
                break;
            }
            /* the two safety rules, at every step of every scenario: a card is never out of reset without power, and a slot that is not allowed is never powered */
            CHECK((ExpSlots_ResetReleasedMask(&slots) & (uint8_t)~ExpSlots_EnableMask(&slots)) == 0, "%s step %u: reset released without power", sc->name, i);
            CHECK((ExpSlots_EnableMask(&slots) & (uint8_t)~cfg.allowed_mask) == 0, "%s step %u: a slot that is not allowed is powered", sc->name, i);
        }
    }

    /* nothing is powered at start, with every slot allowed and every card present */
    {
        ExpSlots_t slots;
        const ExpSlotsConfig cfg = {7, 20, 50, 20, 5, 3};
        CHECK(ExpSlots_Init(&slots, &cfg), "init");
        ExpSlots_Start(&slots, 0);
        CHECK(ExpSlots_EnableMask(&slots) == 0 && ExpSlots_ResetReleasedMask(&slots) == 0, "the slots are on before the first update");
    }
    /* a configuration that cannot work is refused */
    {
        ExpSlots_t slots;
        const ExpSlotsConfig good = {7, 20, 50, 20, 5, 3};
        ExpSlotsConfig bad = good;
        CHECK(ExpSlots_Init(&slots, &good), "the good configuration is accepted");
        bad = good;
        bad.allowed_mask = 8;
        CHECK(!ExpSlots_Init(&slots, &bad), "a mask above the three slots");
        bad = good;
        bad.fault_blank_ms = 0;
        CHECK(!ExpSlots_Init(&slots, &bad), "no blanking time");
        bad = good;
        bad.power_good_ms = 20;
        CHECK(!ExpSlots_Init(&slots, &bad), "power good not above the blanking time");
        bad = good;
        bad.power_good_ms = 5001;
        CHECK(!ExpSlots_Init(&slots, &bad), "power good over 5 s");
        bad = good;
        bad.reset_release_ms = 0;
        CHECK(!ExpSlots_Init(&slots, &bad), "no reset release time");
        bad = good;
        bad.fault_debounce_ms = 1001;
        CHECK(!ExpSlots_Init(&slots, &bad), "debounce over 1 s");
        bad = good;
        bad.max_faults = 0;
        CHECK(!ExpSlots_Init(&slots, &bad), "no faults allowed");
        bad = good;
        bad.max_faults = 21;
        CHECK(!ExpSlots_Init(&slots, &bad), "more than 20 faults");
    }
    /* a command for a slot that does not exist is ignored */
    {
        ExpSlots_t slots;
        const ExpSlotsConfig cfg = {7, 20, 50, 20, 5, 3};
        CHECK(ExpSlots_Init(&slots, &cfg), "init");
        ExpSlots_Start(&slots, 0);
        ExpSlots_ClearFault(&slots, 3);
        ExpSlots_Unlock(&slots, 200, 0);
        CHECK(ExpSlots_EnableMask(&slots) == 0, "a command for a slot that does not exist changed something");
    }
    if (failures) {
        printf("test_expansion_slots: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_expansion_slots: all %u scenarios passed\n", (unsigned)ES_SCENARIO_COUNT);
    return 0;
}
