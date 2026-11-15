/* =============================================================================
 * HYDRA-UMC Firmware - src/mcu_stm32h757/Common/nor_update_flow.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * nor_update_flow.h - the backup of the 768 KB application slot BEFORE an update (HAL-free, host-testable, over the operations of nor_backup_plan.h). With the long slot the update
 * is staged in the NOR (the "current" region, which mirrors the application and ends as the new image), so the old application would be lost the moment the new one is committed.
 * This file keeps a second region of the NOR, "previous", and saves the application that is running into it before the update starts: if the new image does not start, the previous
 * one is put back. The two regions are two NorbConfig (two plans of nor_backup_plan.h) that must not overlap and must fit the NOR. The backup is made only when the application in the
 * flash is good (a broken one is not worth keeping: the update is then the repair); when the backup fails the update is refused unless the configuration says it may go on without it
 * (require_backup). Nuf_Recover picks what to do at start-up: nothing, restore the current copy (a cut of the power in the middle of a commit), or fall back to the previous one.
 * Nuf_RecoverFor is the start-up choice made against the metadata of the application: the current copy is used only when it is the image the metadata describes, so an older copy never
 * replaces a newer application; otherwise the previous application comes back, and the caller learns its length and CRC-32 to describe it again.
 * tools/nor_update_flow.py is the Python twin and reads config/nor_backup.cfg. The use of this flow ships off (HYDRA_NOR_BACKUP_BEFORE_UPDATE 0): the bootloader of the CM7 calls it
 * (src/mcu_stm32h757/CM7/boot/bootloader_nor.c) only in the long-slot build that switches it on. Evidence: simulator; nothing here has run on a board.
 * ============================================================================= */
#ifndef HYDRA_UMC_NOR_UPDATE_FLOW_H
#define HYDRA_UMC_NOR_UPDATE_FLOW_H

#include <stdint.h>

#include "nor_backup_plan.h"

#define NUF_DEFAULT_PREVIOUS_OFFSET 851968UL /* the end of the current region of the defaults: 64 KB + 768 KB */

#define NUF_OK 0
#define NUF_SKIPPED 1  /* the application is not good: there is nothing to back up and the update goes on */
#define NUF_E_ARG 2
#define NUF_E_LAYOUT 3 /* a region does not fit, the regions overlap or their slots differ */
#define NUF_E_BACKUP 4 /* the save of the previous application failed */
#define NUF_E_NONE 5   /* nothing to put back */

#define NUF_ACT_NONE 0
#define NUF_ACT_RESTORE_CURRENT 1
#define NUF_ACT_ROLLBACK_PREVIOUS 2
#define NUF_ACT_LOST 3

typedef struct {
    NorbConfig current;      /* the staging and mirror region */
    NorbConfig previous;     /* the application that ran before the update */
    uint32_t require_backup; /* 1: an update whose backup failed is refused */
} NufConfig;

static inline void Nuf_Defaults(NufConfig *c)
{
    Norb_Defaults(&c->current);
    Norb_Defaults(&c->previous);
    c->previous.nor_offset = NUF_DEFAULT_PREVIOUS_OFFSET;
    c->require_backup = 1UL;
}

/* the layouts of both regions; the regions may not overlap and must hold the same slot */
static inline int Nuf_Plan(const NufConfig *c, NorbLayout *current, NorbLayout *previous)
{
    if (c == 0 || current == 0 || previous == 0) return NUF_E_ARG;
    if (Norb_Plan(&c->current, current) != NORB_OK || Norb_Plan(&c->previous, previous) != NORB_OK) return NUF_E_LAYOUT;
    if (c->current.slot_bytes != c->previous.slot_bytes) return NUF_E_LAYOUT;
    if (previous->header_a < current->end && current->header_a < previous->end) return NUF_E_LAYOUT;
    return NUF_OK;
}

/* Before an update starts: saves the application of the flash (`app_length` bytes) into the previous region. `*allowed` is 1 when the update may go on. */
static inline int Nuf_BackupBeforeUpdate(const NorbOps *ops, const NufConfig *c, int app_good, uint32_t app_length, int *allowed)
{
    NorbLayout current, previous;
    int rc;
    if (allowed == 0) return NUF_E_ARG;
    *allowed = 0;
    if (ops == 0) return NUF_E_ARG;
    rc = Nuf_Plan(c, &current, &previous);
    if (rc != NUF_OK) return rc;
    if (!app_good) {
        *allowed = 1;
        return NUF_SKIPPED;
    }
    if (Norb_Save(ops, &c->previous, app_length) == NORB_OK) {
        *allowed = 1;
        return NUF_OK;
    }
    *allowed = c->require_backup ? 0 : 1;
    return NUF_E_BACKUP;
}

/* what to do at start-up, from three facts */
static inline int Nuf_Decide(int app_good, int current_copy, int previous_copy)
{
    if (app_good) return NUF_ACT_NONE;
    if (current_copy) return NUF_ACT_RESTORE_CURRENT;
    if (previous_copy) return NUF_ACT_ROLLBACK_PREVIOUS;
    return NUF_ACT_LOST;
}

/* Start-up recovery: acts on Nuf_Decide; a current copy that cannot be restored falls back to the previous one. `*used` is the action that ended up working. */
static inline int Nuf_Recover(const NorbOps *ops, const NufConfig *c, int app_good, uint32_t *restored_length, int *used)
{
    NorbLayout current, previous;
    int act, rc;
    if (ops == 0 || used == 0) return NUF_E_ARG;
    *used = NUF_ACT_NONE;
    rc = Nuf_Plan(c, &current, &previous);
    if (rc != NUF_OK) return rc;
    act = Nuf_Decide(app_good, Norb_CopyExists(ops, &c->current), Norb_CopyExists(ops, &c->previous));
    if (act == NUF_ACT_NONE) return NUF_OK;
    if (act == NUF_ACT_RESTORE_CURRENT) {
        if (Norb_Restore(ops, &c->current, restored_length) == NORB_OK) {
            *used = NUF_ACT_RESTORE_CURRENT;
            return NUF_OK;
        }
        act = NUF_ACT_ROLLBACK_PREVIOUS;
    }
    if (act == NUF_ACT_ROLLBACK_PREVIOUS && Norb_Restore(ops, &c->previous, restored_length) == NORB_OK) {
        *used = NUF_ACT_ROLLBACK_PREVIOUS;
        return NUF_OK;
    }
    *used = NUF_ACT_LOST;
    return NUF_E_NONE;
}

/* the image that came back into the flash */
typedef struct {
    uint32_t length;
    uint32_t crc32;
} NufRestored;

/* Start-up recovery against the metadata of the application (`length`, `crc32`): the current copy comes back only when it is exactly that image; a current copy that is older, damaged or
 * refused by the flash gives way to the previous application. `*used` is the action that worked and `out` describes what is now in the flash. */
static inline int Nuf_RecoverFor(const NorbOps *ops, const NufConfig *c, int app_good, uint32_t length, uint32_t crc32, NufRestored *out, int *used)
{
    NorbLayout current, previous;
    NorbHeader h = {0, 0, 0};
    int act, matches, which, rc;
    if (ops == 0 || out == 0 || used == 0) return NUF_E_ARG;
    *used = NUF_ACT_NONE;
    rc = Nuf_Plan(c, &current, &previous);
    if (rc != NUF_OK) return rc;
    matches = Norb_ReadHeaders(ops, &current, c->current.slot_bytes, &h, &which) && h.length == length && h.crc32 == crc32 && Norb_CopyExists(ops, &c->current);
    act = Nuf_Decide(app_good, matches, Norb_CopyExists(ops, &c->previous));
    if (act == NUF_ACT_NONE) return NUF_OK;
    if (act == NUF_ACT_RESTORE_CURRENT) {
        if (Norb_Restore(ops, &c->current, &out->length) == NORB_OK) {
            out->crc32 = crc32;
            *used = NUF_ACT_RESTORE_CURRENT;
            return NUF_OK;
        }
        act = NUF_ACT_ROLLBACK_PREVIOUS;
    }
    if (act == NUF_ACT_ROLLBACK_PREVIOUS && Norb_Restore(ops, &c->previous, &out->length) == NORB_OK) {
        (void)Norb_ReadHeaders(ops, &previous, c->previous.slot_bytes, &h, &which);
        out->crc32 = h.crc32;
        *used = NUF_ACT_ROLLBACK_PREVIOUS;
        return NUF_OK;
    }
    *used = NUF_ACT_LOST;
    return NUF_E_NONE;
}

#endif /* HYDRA_UMC_NOR_UPDATE_FLOW_H */
