/* =============================================================================
 * HYDRA-UMC Firmware - src/mcu_stm32h757/Common/expansion_slots.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * expansion_slots.h - the power sequence and fault handling of the three expansion slots (HAL-free, host-testable). The C twin of tools/expansion_slots.py (read its header and
 * docs/EXPANSION_SLOTS.md). The caller reads the present and fault lines of the three slots into two masks (bit set: card in / fuse reports a fault), calls ExpSlots_Update with the time,
 * and drives the enable and reset lines from ExpSlots_EnableMask and ExpSlots_ResetReleasedMask. Fail-safe: everything starts off with the cards in reset, a slot is powered only when it is
 * allowed in the configuration AND a card is present, power comes before the release of reset, a fault cuts the slot until a person clears it, and a slot that faults too often is locked.
 * tests/test_expansion_slots.c runs it against the vectors written from the Python reference. Nothing here has run on a board.
 * ============================================================================= */
#ifndef HYDRA_UMC_EXPANSION_SLOTS_H
#define HYDRA_UMC_EXPANSION_SLOTS_H

#include <stdint.h>

#define EXP_SLOTS 3u

#define EXP_ST_OFF 0u
#define EXP_ST_ABSENT 1u
#define EXP_ST_POWERING 2u
#define EXP_ST_RESETTING 3u
#define EXP_ST_READY 4u
#define EXP_ST_FAULT 5u
#define EXP_ST_LOCKED 6u

typedef struct {
    uint8_t allowed_mask;
    uint32_t fault_blank_ms;
    uint32_t power_good_ms;
    uint32_t reset_release_ms;
    uint32_t fault_debounce_ms;
    uint8_t max_faults;
} ExpSlotsConfig;

typedef struct {
    uint8_t state;
    uint8_t enable;
    uint8_t reset_released;
    uint32_t since;
    uint32_t fault_since;
    uint8_t fault_seen;
    uint8_t faults;
    uint8_t clear_requested;
} ExpSlot_t;

typedef struct {
    ExpSlotsConfig cfg;
    ExpSlot_t slot[EXP_SLOTS];
} ExpSlots_t;

/* Returns 0 when the configuration cannot work (the same limits as config/expansion_slots.cfg). */
static inline uint8_t ExpSlots_Init(ExpSlots_t *s, const ExpSlotsConfig *cfg)
{
    if (cfg->allowed_mask >= (1u << EXP_SLOTS)) return 0;
    if (cfg->fault_blank_ms < 1u || cfg->fault_blank_ms > 1000u || cfg->power_good_ms <= cfg->fault_blank_ms || cfg->power_good_ms > 5000u) return 0;
    if (cfg->reset_release_ms < 1u || cfg->reset_release_ms > 1000u || cfg->fault_debounce_ms > 1000u || cfg->max_faults < 1u || cfg->max_faults > 20u) return 0;
    s->cfg = *cfg;
    for (uint32_t i = 0; i < EXP_SLOTS; i++) {
        s->slot[i] = (ExpSlot_t){0};
    }
    return 1;
}

static inline void ExpSlots_Cut(ExpSlot_t *slot, uint8_t state, uint32_t now)
{
    slot->enable = 0;
    slot->reset_released = 0;
    slot->state = state;
    slot->since = now;
}

static inline void ExpSlots_Start(ExpSlots_t *s, uint32_t now)
{
    for (uint32_t i = 0; i < EXP_SLOTS; i++) {
        ExpSlot_t *slot = &s->slot[i];
        slot->state = EXP_ST_OFF;
        slot->enable = 0;
        slot->reset_released = 0;
        slot->since = now;
        slot->faults = 0;
        slot->clear_requested = 0;
        if ((s->cfg.allowed_mask >> i) & 1u) slot->state = EXP_ST_ABSENT;
    }
}

static inline void ExpSlots_ClearFault(ExpSlots_t *s, uint8_t index)
{
    if (index < EXP_SLOTS && s->slot[index].state == EXP_ST_FAULT) s->slot[index].clear_requested = 1;
}

static inline void ExpSlots_Unlock(ExpSlots_t *s, uint8_t index, uint32_t now)
{
    if (index < EXP_SLOTS && s->slot[index].state == EXP_ST_LOCKED) {
        s->slot[index].faults = 0;
        s->slot[index].clear_requested = 0;
        ExpSlots_Cut(&s->slot[index], EXP_ST_ABSENT, now);
    }
}

static inline void ExpSlots_Trip(ExpSlots_t *s, ExpSlot_t *slot, uint32_t now)
{
    slot->faults++;
    ExpSlots_Cut(slot, slot->faults >= s->cfg.max_faults ? EXP_ST_LOCKED : EXP_ST_FAULT, now);
    slot->fault_seen = 0;
    slot->clear_requested = 0;
}

static inline void ExpSlots_Update(ExpSlots_t *s, uint32_t now, uint8_t present, uint8_t fault)
{
    for (uint32_t i = 0; i < EXP_SLOTS; i++) {
        ExpSlot_t *slot = &s->slot[i];
        const uint8_t allowed = (uint8_t)((s->cfg.allowed_mask >> i) & 1u);
        const uint8_t has_card = (uint8_t)((present >> i) & 1u);
        const uint8_t has_fault = (uint8_t)((fault >> i) & 1u);
        const uint32_t elapsed = (uint32_t)(now - slot->since);
        if (!allowed) {
            if (slot->state != EXP_ST_OFF) ExpSlots_Cut(slot, EXP_ST_OFF, now);
            continue;
        }
        if (slot->state == EXP_ST_OFF) ExpSlots_Cut(slot, EXP_ST_ABSENT, now);
        if ((slot->state == EXP_ST_POWERING || slot->state == EXP_ST_RESETTING || slot->state == EXP_ST_READY) && !has_card) {
            ExpSlots_Cut(slot, EXP_ST_ABSENT, now);
            continue;
        }
        if (slot->state == EXP_ST_ABSENT) {
            if (has_card) {
                slot->enable = 1;
                slot->reset_released = 0;
                slot->state = EXP_ST_POWERING;
                slot->since = now;
                slot->fault_seen = 0;
            }
        } else if (slot->state == EXP_ST_POWERING) {
            if (elapsed >= s->cfg.fault_blank_ms && has_fault) {
                ExpSlots_Trip(s, slot, now);
            } else if (elapsed >= s->cfg.power_good_ms) {
                slot->reset_released = 1;
                slot->state = EXP_ST_RESETTING;
                slot->since = now;
            }
        } else if (slot->state == EXP_ST_RESETTING) {
            if (has_fault) {
                ExpSlots_Trip(s, slot, now);
            } else if (elapsed >= s->cfg.reset_release_ms) {
                slot->state = EXP_ST_READY;
                slot->since = now;
                slot->fault_seen = 0;
            }
        } else if (slot->state == EXP_ST_READY) {
            if (has_fault) {
                if (!slot->fault_seen) {
                    slot->fault_seen = 1;
                    slot->fault_since = now;
                }
                if ((uint32_t)(now - slot->fault_since) >= s->cfg.fault_debounce_ms) ExpSlots_Trip(s, slot, now);
            } else {
                slot->fault_seen = 0;
            }
        } else if (slot->state == EXP_ST_FAULT) {
            if (slot->clear_requested && !has_fault) {
                slot->clear_requested = 0;
                ExpSlots_Cut(slot, EXP_ST_ABSENT, now);
            }
        }
    }
}

static inline uint8_t ExpSlots_EnableMask(const ExpSlots_t *s)
{
    uint8_t mask = 0;
    for (uint32_t i = 0; i < EXP_SLOTS; i++) mask = (uint8_t)(mask | (s->slot[i].enable << i));
    return mask;
}

static inline uint8_t ExpSlots_ResetReleasedMask(const ExpSlots_t *s)
{
    uint8_t mask = 0;
    for (uint32_t i = 0; i < EXP_SLOTS; i++) mask = (uint8_t)(mask | (s->slot[i].reset_released << i));
    return mask;
}

#endif /* HYDRA_UMC_EXPANSION_SLOTS_H */
