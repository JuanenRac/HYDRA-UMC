/* =============================================================================
 * HYDRA-UMC Firmware - src/mcu_stm32h757/Common/boot_nor_stage.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * boot_nor_stage.h - what the protocol code of the CM7 bootloader asks of the staging area in the NOR flash when the application slot is 768 KB (HAL-free, host-testable). The bootloader
 * used to stage an update in the second half of bank 1 and copy it over the application; with the long slot that half is the application, so every one of those steps goes to the NOR
 * through nor_backup_plan.h instead: begin (erase the image area), one page at a time, the digest of what was staged (streamed in chunks to a sink, because the NOR is not memory
 * mapped), the first two words of the image, finish (the CRC-32 of the staged image is checked and only then the header is written) and commit (the staged image is checked again and
 * written over the application slot, then read back). Recover is the boot-time rule: an application that is not good is replaced by the copy only when the copy is the image the metadata
 * describes (same length and CRC-32), so an older copy never replaces a newer application that was damaged.
 *
 * Header-only and free of the HAL: the host test runs it against RAM doubles of the NOR and the flash; on the board the operations come from nor_backup_glue.h over CM7/NorSpi5.c.
 * The use ships OFF (HYDRA_NOR_BACKUP 0): the bootloader includes this file only when the switch is 1. tools/boot_nor_stage.py is the Python twin. Evidence: simulator; nothing here
 * has run on a board.
 * ============================================================================= */
#ifndef HYDRA_UMC_BOOT_NOR_STAGE_H
#define HYDRA_UMC_BOOT_NOR_STAGE_H

#include <stdint.h>

#include "nor_backup_plan.h"

#define BNS_OK 0
#define BNS_E_ARG 20       /* a null pointer or an address that does not fit the staged image */
#define BNS_E_MISMATCH 21  /* the copy in the NOR is not the image the caller expects (length or CRC-32) */
#define BNS_E_SINK 22      /* the consumer of the digest stream refused a chunk */
#define BNS_RECOVER_NONE 0 /* the application is good: nothing was done */
#define BNS_RECOVER_DONE 1 /* the application was not good and the copy was written over it */
#define BNS_RECOVER_LOST 2 /* the application is not good and there is no copy that matches */

typedef struct {
    NorbConfig cfg;
    NorbOps ops;
} BootNorStage;

/* the consumer of the streamed image: returns 0 to go on */
typedef int (*BootNorSink)(void *ctx, const uint8_t *data, uint32_t length);

static inline void BootNorStage_Init(BootNorStage *s, const NorbConfig *cfg, const NorbOps *ops)
{
    s->cfg = *cfg;
    s->ops = *ops;
}

/* an update of `total` bytes is about to arrive: the image area is erased and any older header stops matching */
static inline int BootNorStage_Begin(const BootNorStage *s, uint32_t total)
{
    if (s == 0) return BNS_E_ARG;
    return Norb_StageBegin(&s->ops, &s->cfg, total);
}

/* page number `index` of `page_bytes` bytes (the last one may be shorter) */
static inline int BootNorStage_Page(const BootNorStage *s, uint32_t index, uint32_t page_bytes, const uint8_t *data, uint32_t length)
{
    if (s == 0 || data == 0 || page_bytes == 0UL) return BNS_E_ARG;
    if (index > s->cfg.slot_bytes / page_bytes) return BNS_E_ARG;
    return Norb_StagePage(&s->ops, &s->cfg, index * page_bytes, data, length);
}

/* `length` bytes of the staged image from `offset`, for the first two words of the vector table and for the digest */
static inline int BootNorStage_Read(const BootNorStage *s, uint32_t offset, uint8_t *data, uint32_t length)
{
    NorbLayout l;
    int rc;
    if (s == 0 || data == 0) return BNS_E_ARG;
    rc = Norb_Plan(&s->cfg, &l);
    if (rc != NORB_OK) return rc;
    if (length == 0UL || offset > s->cfg.slot_bytes || length > s->cfg.slot_bytes - offset) return BNS_E_ARG;
    return s->ops.nor_read(s->ops.ctx, l.image_offset + offset, data, length) == 0 ? NORB_OK : NORB_E_NOR_READ;
}

/* the first `total` bytes of the staged image, in chunks, to the sink (a SHA-256 in the bootloader) */
static inline int BootNorStage_Stream(const BootNorStage *s, uint32_t total, BootNorSink sink, void *sink_ctx)
{
    uint8_t buf[NORB_CHUNK];
    uint32_t done = 0, n;
    int rc;
    if (sink == 0) return BNS_E_ARG;
    while (done < total) {
        n = (total - done < NORB_CHUNK) ? total - done : NORB_CHUNK;
        rc = BootNorStage_Read(s, done, buf, n);
        if (rc != NORB_OK) return rc;
        if (sink(sink_ctx, buf, n) != 0) return BNS_E_SINK;
        done += n;
    }
    return BNS_OK;
}

/* the whole update arrived and verified: the CRC-32 of the staged image must be `crc32`, and only then the header is written */
static inline int BootNorStage_Finish(const BootNorStage *s, uint32_t total, uint32_t crc32)
{
    if (s == 0) return BNS_E_ARG;
    return Norb_StageFinish(&s->ops, &s->cfg, total, crc32);
}

/* 1 when the newest good header of the NOR describes exactly this image */
static inline int BootNorStage_CopyIs(const BootNorStage *s, uint32_t length, uint32_t crc32)
{
    NorbLayout l;
    NorbHeader h;
    int which;
    if (s == 0 || Norb_Plan(&s->cfg, &l) != NORB_OK) return 0;
    if (!Norb_ReadHeaders(&s->ops, &l, s->cfg.slot_bytes, &h, &which)) return 0;
    return h.length == length && h.crc32 == crc32;
}

/* writes the staged image over the application slot: refused unless the copy is the image of `length` and `crc32`; the plan checks the NOR first and reads the flash back after */
static inline int BootNorStage_Commit(const BootNorStage *s, uint32_t length, uint32_t crc32)
{
    uint32_t restored = 0;
    int rc;
    if (s == 0) return BNS_E_ARG;
    if (!BootNorStage_CopyIs(s, length, crc32)) return BNS_E_MISMATCH;
    rc = Norb_Restore(&s->ops, &s->cfg, &restored);
    if (rc != NORB_OK) return rc;
    return restored == length ? BNS_OK : BNS_E_MISMATCH;
}

/* the boot-time rule. `*action` says what happened (BNS_RECOVER_*); the return value is NORB_OK / BNS_OK unless a restore was tried and failed. */
static inline int BootNorStage_Recover(const BootNorStage *s, int application_good, uint32_t length, uint32_t crc32, int *action)
{
    int copy;
    if (s == 0 || action == 0) return BNS_E_ARG;
    copy = BootNorStage_CopyIs(s, length, crc32) && Norb_CopyExists(&s->ops, &s->cfg);
    switch (Norb_Decide(application_good, copy)) {
    case NORB_ACT_NONE:
        *action = BNS_RECOVER_NONE;
        return BNS_OK;
    case NORB_ACT_RESTORE:
        *action = BNS_RECOVER_DONE;
        return BootNorStage_Commit(s, length, crc32);
    default:
        *action = BNS_RECOVER_LOST;
        return BNS_OK;
    }
}

#endif /* HYDRA_UMC_BOOT_NOR_STAGE_H */
