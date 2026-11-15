// =============================================================================
// HYDRA-UMC Kinematic Brain Bootloader (CM7) - bootloader_nor.c: SPI5 and the flash layer under the NOR staging; see bootloader_nor.h
// Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
// GPL-3.0-or-later - see repo root LICENSE
// =============================================================================
#include "bootloader_nor.h"

#if HYDRA_NOR_BACKUP

#include "stm32h7xx_hal.h"
#include <string.h>
#include "bootloader_common.h"
#include "bootloader_crypto.h"
#include "bootloader_flash.h"
#include "NorSpi5.h"
#include "boot_nor_stage.h"
#include "nor_backup_glue.h"
#include "nor_spi5_pins.h"
#if HYDRA_NOR_BACKUP_BEFORE_UPDATE
#include "nor_update_flow.h"
#endif

extern IWDG_HandleTypeDef hiwdg;

static BootNorStage g_stage;
static NorbGlue g_glue;
static NorbOps g_ops;
#if HYDRA_NOR_BACKUP_BEFORE_UPDATE
static NufConfig g_flow;           // the current region (the staging) and the previous one (the application that ran before the update)
#endif
static NorOps g_fed_ops;           // the operations of SPI5 with a watchdog refresh in front of every transfer: an erase of the whole image area takes seconds
static const NorOps *g_chip_ops;
static uint8_t g_state;            // 0 not tried, 1 open, 2 refused (the NOR did not answer); a refusal is not retried inside the same session

static int FedTransfer(void *ctx, const uint8_t *tx, uint8_t *rx, uint32_t length) {
    HAL_IWDG_Refresh(&hiwdg);
    return g_chip_ops->transfer(ctx, tx, rx, length);
}

static int AppRead(void *ctx, uint32_t offset, uint8_t *data, uint32_t length) {
    (void)ctx;
    memcpy(data, (const void*)(MAIN_APP_ADDR + offset), length);
    return 0;
}

static int AppErase(void *ctx, uint32_t length) {
    (void)ctx;
    return Flash_ErasePages(MAIN_APP_ADDR, (length + FLASH_PAGE_SIZE - 1UL) / FLASH_PAGE_SIZE) ? 0 : 1;
}

static int AppWrite(void *ctx, uint32_t offset, const uint8_t *data, uint32_t length) {
    (void)ctx;
    HAL_IWDG_Refresh(&hiwdg);
    return Flash_WriteVerified(MAIN_APP_ADDR + offset, data, length) ? 0 : 1;
}

// GPIOA..GPIOK sit at the same distance from each other on this chip.
static GPIO_TypeDef *PortOf(uint8_t index) {
    return (GPIO_TypeDef*)(GPIOA_BASE + (uint32_t)index * (GPIOB_BASE - GPIOA_BASE));
}

static uint8_t Open(void) {
    NorS5Pins pins;
    NorSpi5Config spi;
    NorbConfig plan;
    NorbFlashLayer layer = {AppRead, AppErase, AppWrite, 0};
    if (g_state == 1) return 1;
    if (g_state == 2) return 0;
    g_state = 2;
    NorS5_Defaults(&pins);
    if (!NorS5_Valid(&pins)) return 0;
    spi.cs_port = PortOf(pins.cs_port);
    spi.cs_pin = (uint16_t)(1U << pins.cs_pin);
    spi.bus_port = PortOf(pins.bus_port);
    spi.bus_pins = NorS5_BusMask(&pins);
    spi.alternate = pins.alternate;
    spi.transfer_timeout_ms = pins.timeout_ms;
    if (!NorSpi5_Init(&spi)) return 0;
    g_chip_ops = NorSpi5_Ops();
    g_fed_ops.select = g_chip_ops->select;
    g_fed_ops.transfer = FedTransfer;
    if (NorbGlue_Open(&g_glue, &g_fed_ops, 0, &layer, &g_ops) != NOR_OK) return 0;
    Norb_Defaults(&plan);
    BootNorStage_Init(&g_stage, &plan, &g_ops);
#if HYDRA_NOR_BACKUP_BEFORE_UPDATE
    Nuf_Defaults(&g_flow);
#endif
    g_state = 1;
    return 1;
}

uint8_t BootNor_Begin(uint32_t total) {
    return Open() && BootNorStage_Begin(&g_stage, total) == NORB_OK;
}

uint8_t BootNor_Page(uint32_t index, const uint8_t *data, uint32_t length) {
    return Open() && BootNorStage_Page(&g_stage, index, FLASH_PAGE_SIZE, data, length) == NORB_OK;
}

uint32_t BootNor_Word(uint32_t offset) {
    uint8_t raw[4];
    if (!Open() || BootNorStage_Read(&g_stage, offset, raw, sizeof(raw)) != NORB_OK) return 0;
    return (uint32_t)raw[0] | ((uint32_t)raw[1] << 8) | ((uint32_t)raw[2] << 16) | ((uint32_t)raw[3] << 24);
}

static int DigestSink(void *ctx, const uint8_t *data, uint32_t length) {
    HAL_IWDG_Refresh(&hiwdg);
    sha256_stream_update((Sha256Stream_t*)ctx, data, length);
    return 0;
}

uint8_t BootNor_Digest(const uint8_t *prefix, uint32_t prefix_len, uint32_t total, uint8_t out[32]) {
    Sha256Stream_t stream;
    memset(out, 0, 32);
    if (!Open()) return 0;
    sha256_stream_begin(&stream);
    if (prefix_len > 0) sha256_stream_update(&stream, prefix, prefix_len);
    if (BootNorStage_Stream(&g_stage, total, DigestSink, &stream) != BNS_OK) return 0;
    sha256_stream_end(&stream, out);
    return 1;
}

uint8_t BootNor_Finish(uint32_t total, uint32_t crc32) {
    return Open() && BootNorStage_Finish(&g_stage, total, crc32) == NORB_OK;
}

uint8_t BootNor_Commit(uint32_t total, uint32_t crc32) {
    return Open() && BootNorStage_Commit(&g_stage, total, crc32) == BNS_OK;
}

uint8_t BootNor_RestoreIfLost(uint32_t size, uint32_t crc32) {
    int action = BNS_RECOVER_NONE;
    if (!Open()) return 0;
    if (BootNorStage_Recover(&g_stage, 0, size, crc32, &action) != BNS_OK) return 0;
    return action == BNS_RECOVER_DONE;
}

#if HYDRA_NOR_BACKUP_BEFORE_UPDATE
uint8_t BootNor_BackupBeforeUpdate(uint8_t app_good, uint32_t app_length, uint32_t version_major, uint32_t version_minor, uint32_t version_patch) {
    int allowed = 0;
    if (!Open()) return 0;   // the staging would be refused as well: the NOR does not answer
    (void)Nuf_BackupBeforeUpdate(&g_ops, &g_flow, app_good, app_length, Nuf_VersionPack(version_major, version_minor, version_patch), &allowed);
    return allowed ? 1 : 0;
}

uint8_t BootNor_RecoverAtStart(uint32_t size, uint32_t crc32, uint32_t *length, uint32_t *crc, uint32_t version[3], uint8_t *rolled_back) {
    NufRestored restored;
    int used = NUF_ACT_NONE;
    if (!Open()) return 0;
    if (Nuf_RecoverFor(&g_ops, &g_flow, 0, size, crc32, &restored, &used) != NUF_OK) return 0;
    *length = restored.length;
    *crc = restored.crc32;
    Nuf_VersionUnpack(restored.version, &version[0], &version[1], &version[2]);
    *rolled_back = (used == NUF_ACT_ROLLBACK_PREVIOUS) ? 1 : 0;
    return 1;
}
#endif // HYDRA_NOR_BACKUP_BEFORE_UPDATE

#endif // HYDRA_NOR_BACKUP
