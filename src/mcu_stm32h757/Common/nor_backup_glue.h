/* =============================================================================
 * HYDRA-UMC Firmware - src/mcu_stm32h757/Common/nor_backup_glue.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * nor_backup_glue.h - puts the copy of the 768 KB slot (nor_backup_plan.h) on the NOR flash of SPI5 (nor_flash.h): the NorbOps of the plan, built from the NorOps of the chip and from
 * the three operations of the flash layer of the bootloader (read, erase and write of the application slot). The NOR offsets are those of the plan, from the start of the chip.
 * Erases go sector by sector (4 KB) from an address that must be the start of a sector, and every write is read back. Header-only and free of the HAL, so the host test runs it against
 * a RAM double of the chip; on the board the NorOps come from CM7/NorSpi5.c. OFF by default: nothing in the bootloader includes this file unless HYDRA_NOR_BACKUP is 1.
 * Evidence: simulator; nothing here has run on a board.
 * ============================================================================= */
#ifndef HYDRA_UMC_NOR_BACKUP_GLUE_H
#define HYDRA_UMC_NOR_BACKUP_GLUE_H

#include <stdint.h>

#include "nor_backup_plan.h"
#include "nor_flash.h"

#if defined(HYDRA_PLATFORM_H757)

typedef struct {
    int (*read)(void *ctx, uint32_t offset, uint8_t *data, uint32_t length);        /* the application slot, from its start */
    int (*erase)(void *ctx, uint32_t length);
    int (*write)(void *ctx, uint32_t offset, const uint8_t *data, uint32_t length);
    void *ctx;
} NorbFlashLayer;

typedef struct {
    const NorOps *nor;
    void *nor_ctx;
    NorbFlashLayer flash;
    uint8_t scratch[HYDRA_NOR_PAGE_BYTES];
} NorbGlue;

static inline int NorbGlue_NorRead(void *ctx, uint32_t offset, uint8_t *data, uint32_t length)
{
    NorbGlue *g = (NorbGlue *)ctx;
    return Nor_Read(g->nor, g->nor_ctx, offset, data, length) == NOR_OK ? 0 : 1;
}

static inline int NorbGlue_NorErase(void *ctx, uint32_t offset, uint32_t length)
{
    NorbGlue *g = (NorbGlue *)ctx;
    uint32_t done;
    if (offset % HYDRA_NOR_SECTOR_BYTES != 0u) return 1;
    for (done = 0; done < length; done += HYDRA_NOR_SECTOR_BYTES) {
        if (Nor_EraseSector(g->nor, g->nor_ctx, offset + done) != NOR_OK) return 1;
    }
    return 0;
}

static inline int NorbGlue_NorWrite(void *ctx, uint32_t offset, const uint8_t *data, uint32_t length)
{
    NorbGlue *g = (NorbGlue *)ctx;
    return Nor_Write(g->nor, g->nor_ctx, offset, data, length, 1, g->scratch) == NOR_OK ? 0 : 1;
}

static inline int NorbGlue_FlashRead(void *ctx, uint32_t offset, uint8_t *data, uint32_t length)
{
    NorbGlue *g = (NorbGlue *)ctx;
    return g->flash.read(g->flash.ctx, offset, data, length);
}

static inline int NorbGlue_FlashErase(void *ctx, uint32_t length)
{
    NorbGlue *g = (NorbGlue *)ctx;
    return g->flash.erase(g->flash.ctx, length);
}

static inline int NorbGlue_FlashWrite(void *ctx, uint32_t offset, const uint8_t *data, uint32_t length)
{
    NorbGlue *g = (NorbGlue *)ctx;
    return g->flash.write(g->flash.ctx, offset, data, length);
}

/* The NorbOps of the plan over this glue; `glue` must outlive them. The chip is probed first: a wrong or absent chip gives NOR_ERR_ID and no operation is returned. */
static inline NorStatus NorbGlue_Open(NorbGlue *glue, const NorOps *nor, void *nor_ctx, const NorbFlashLayer *flash, NorbOps *out)
{
    const NorStatus status = Nor_Probe(nor, nor_ctx);
    if (status != NOR_OK) return status;
    glue->nor = nor;
    glue->nor_ctx = nor_ctx;
    glue->flash = *flash;
    out->nor_read = NorbGlue_NorRead;
    out->nor_erase = NorbGlue_NorErase;
    out->nor_write = NorbGlue_NorWrite;
    out->flash_read = NorbGlue_FlashRead;
    out->flash_erase = NorbGlue_FlashErase;
    out->flash_write = NorbGlue_FlashWrite;
    out->ctx = glue;
    return NOR_OK;
}

#endif /* HYDRA_PLATFORM_H757 */

#endif /* HYDRA_UMC_NOR_BACKUP_GLUE_H */
