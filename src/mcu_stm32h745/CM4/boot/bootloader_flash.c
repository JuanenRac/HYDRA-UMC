// =============================================================================
// HYDRA-UMC Kinematic Brain Bootloader (CM4) - CRC32, flash program/erase,
// and metadata persistence
// Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
// GPL-3.0 - see repo root LICENSE
//
// Identical structure to ../../CM7/boot/bootloader_flash.c (see that
// file's own header for the full reasoning on sector-vs-page erase
// granularity and FLASHWORD programming) - this is Bank 2 instead of
// Bank 1, the only real difference. Kept as a separate file rather than a
// shared one because each core's bootloader is its own independent build
// target (own .elf, own flash bank) - see ../../../build_firmware.sh.
// =============================================================================
#include "stm32h7xx_hal.h"
#include <string.h>
#include "bootloader_common.h"
#include "bootloader_flash.h"

static uint32_t crc32_table[256];
static uint8_t crc32_table_built = 0;

static void CRC32_BuildTable(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (uint8_t k = 0; k < 8; k++) {
            c = (c & 1) ? (0xEDB88320UL ^ (c >> 1)) : (c >> 1);
        }
        crc32_table[i] = c;
    }
    crc32_table_built = 1;
}

uint32_t CRC32_Update(uint32_t crc, const uint8_t *data, uint32_t len) {
    if (!crc32_table_built) CRC32_BuildTable();
    for (uint32_t i = 0; i < len; i++) {
        crc = crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc;
}

uint32_t CRC32_Finalize(uint32_t crc) {
    return crc ^ 0xFFFFFFFFUL;
}

static uint8_t EraseSectorsForRange(uint32_t start_addr, uint32_t total_len) {
    uint32_t end_addr = start_addr + total_len;
    uint32_t first_sector = (start_addr - BANK2_BASE) / FLASH_SECTOR_SIZE;
    uint32_t last_sector = (end_addr - 1 - BANK2_BASE) / FLASH_SECTOR_SIZE;

    for (uint32_t s = first_sector; s <= last_sector; s++) {
        FLASH_EraseInitTypeDef eraseInit;
        uint32_t sectorError;
        eraseInit.TypeErase = FLASH_TYPEERASE_SECTORS;
        eraseInit.Banks = FLASH_BANK_2;
        eraseInit.Sector = s;
        eraseInit.NbSectors = 1;
        eraseInit.VoltageRange = FLASH_VOLTAGE_RANGE_3;

        HAL_FLASH_Unlock();
        HAL_StatusTypeDef res = HAL_FLASHEx_Erase(&eraseInit, &sectorError);
        HAL_FLASH_Lock();
        if (res != HAL_OK || sectorError != 0xFFFFFFFF) return 0;
        HAL_IWDG_Refresh(&hiwdg);
    }
    return 1;
}

uint8_t Flash_ErasePages(uint32_t start_addr, uint32_t num_pages) {
    return EraseSectorsForRange(start_addr, num_pages * FLASH_PAGE_SIZE);
}

uint8_t Flash_WriteVerified(uint32_t addr, const uint8_t *buf, uint32_t len) {
    HAL_FLASH_Unlock();
    for (uint32_t i = 0; i < len; i += 32) {
        uint32_t word[8];
        memset(word, 0xFF, sizeof(word));
        uint32_t chunk = (len - i < 32) ? (len - i) : 32;
        memcpy(word, &buf[i], chunk);
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_FLASHWORD, addr + i, (uint32_t)(uintptr_t)word) != HAL_OK) {
            HAL_FLASH_Lock();
            return 0;
        }
        if ((i & 0xFFF) == 0) HAL_IWDG_Refresh(&hiwdg);
    }
    HAL_FLASH_Lock();
    __DSB();

    for (uint32_t i = 0; i < len; i += 32) {
        uint32_t expected[8];
        memset(expected, 0xFF, sizeof(expected));
        uint32_t chunk = (len - i < 32) ? (len - i) : 32;
        memcpy(expected, &buf[i], chunk);
        for (uint32_t w = 0; w < 8; w++) {
            uint32_t actual = *(volatile uint32_t*)(addr + i + w * 4);
            if (actual != expected[w]) return 0;
        }
        if ((i & 0xFFF) == 0) HAL_IWDG_Refresh(&hiwdg);
    }
    return 1;
}

uint8_t Flash_CopyRegion(uint32_t dest_addr, uint32_t src_addr, uint32_t total_len) {
    if (!EraseSectorsForRange(dest_addr, total_len)) return 0;

    uint32_t num_pages = (total_len + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE;
    for (uint32_t p = 0; p < num_pages; p++) {
        uint32_t page_dest = dest_addr + (p * FLASH_PAGE_SIZE);
        uint32_t page_src  = src_addr  + (p * FLASH_PAGE_SIZE);
        uint32_t this_page_len = FLASH_PAGE_SIZE;
        if ((p + 1) * FLASH_PAGE_SIZE > total_len) {
            this_page_len = total_len - (p * FLASH_PAGE_SIZE);
        }

        if (!Flash_WriteVerified(page_dest, (const uint8_t*)page_src, this_page_len)) return 0;
        HAL_IWDG_Refresh(&hiwdg);

        uint8_t pct = (uint8_t)(((uint64_t)(p + 1) * 100) / num_pages);
        CAN_SendHeartbeat(STATUS_COPYING, pct);
    }
    return 1;
}

uint8_t Metadata_Read(FirmwareMetadata_t *out) {
    memcpy(out, (const void*)METADATA_ADDR, sizeof(FirmwareMetadata_t));
    return out->magic == METADATA_MAGIC_VALID;
}

static uint8_t Metadata_Write(const FirmwareMetadata_t *meta) {
    return Flash_WriteVerified(METADATA_ADDR, (const uint8_t*)meta, sizeof(FirmwareMetadata_t));
}

uint8_t Metadata_EraseAndWrite(const FirmwareMetadata_t *meta) {
    FirmwareMetadata_t current;
    if (Metadata_Read(&current) && memcmp(&current, meta, sizeof(FirmwareMetadata_t)) == 0) {
        return 1;
    }
    if (!EraseSectorsForRange(METADATA_ADDR, 1)) return 0;
    return Metadata_Write(meta);
}
