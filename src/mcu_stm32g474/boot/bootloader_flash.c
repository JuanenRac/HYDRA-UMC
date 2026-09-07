// =============================================================================
// HYDRA-UMC Robot Controller Board Bootloader - CRC32, flash program/erase,
// and metadata persistence
// Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
// GPL-3.0 - see repo root LICENSE
//
// Ported from URTC's own proven bootloader_flash.c, re-targeted for this
// chip's real flash controller differences from the F303's:
//   - STM32G4 programs in 64-bit DOUBLE-WORDS (FLASH_TYPEPROGRAM_DOUBLEWORD),
//     not 16-bit half-words - every write address here must be 8-byte
//     aligned, which FLASH_PAGE_SIZE (2048) and every call site's own
//     page-granular addressing already guarantee.
//   - STM32G4's FLASH_EraseInitTypeDef erases by PAGE NUMBER (0-255 for this
//     512KB part in single-bank/DBANK=0 mode - see the option-bytes note
//     below), not by page ADDRESS the way F3's HAL takes it - AddrToPage()
//     below does that conversion once, in one place.
//   - Assumes DBANK=0 (single 512KB bank, one flat page-number space
//     0-255) in the option bytes - this is the option-byte configuration
//     this file's own page-number math depends on; if a future revision
//     changes that, this file's AddrToPage()/Banks field both need
//     revisiting together, not just one.
// =============================================================================
#include "stm32g4xx_hal.h"
#include <string.h>
#include "bootloader_common.h"
#include "bootloader_flash.h"

static uint32_t crc32_table[256];
static uint8_t crc32_table_built = 0;

// Software CRC32 (standard polynomial 0xEDB88320, reflected, final XOR
// 0xFFFFFFFF) - same zlib/PNG/Ethernet variant URTC's own bootloader uses,
// deliberately NOT this chip's own hardware CRC unit (different variant by
// default, no reflection/final-XOR) for the same reason URTC avoids it:
// whatever tool generates update images should be able to use a standard
// library call (e.g. Python's zlib.crc32) rather than replicate a
// hardware-specific variant.
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

// -----------------------------------------------------------------------
// Flash routines
// -----------------------------------------------------------------------

// FLASH_BASE is CMSIS-provided; this part's flash starts at 0x08000000.
static uint32_t AddrToPage(uint32_t addr) {
    return (addr - FLASH_BASE) / FLASH_PAGE_SIZE;
}

uint8_t Flash_ErasePages(uint32_t start_addr, uint32_t num_pages) {
    // One page at a time with an IWDG refresh between each - same reasoning
    // as URTC's own bootloader: a single HAL_FLASHEx_Erase call spanning
    // every page at once loops internally with nothing able to refresh the
    // watchdog until it fully returns, and 236KB (118 pages) could plausibly
    // run long enough on its own to trip it.
    for (uint32_t i = 0; i < num_pages; i++) {
        FLASH_EraseInitTypeDef eraseInit;
        uint32_t pageError;
        eraseInit.TypeErase = FLASH_TYPEERASE_PAGES;
        eraseInit.Banks = FLASH_BANK_1;
        eraseInit.Page = AddrToPage(start_addr) + i;
        eraseInit.NbPages = 1;

        HAL_FLASH_Unlock();
        __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
        HAL_StatusTypeDef res = HAL_FLASHEx_Erase(&eraseInit, &pageError);
        HAL_FLASH_Lock();
        if (res != HAL_OK || pageError != 0xFFFFFFFF) return 0;
        HAL_IWDG_Refresh(&hiwdg);
    }
    return 1;
}

// Writes len bytes (rounded up to a double-word, i.e. 8 bytes) starting at
// addr, then reads every double-word back and compares it against the
// source buffer before reporting success - same "catch a stuck flash cell
// at write time" reasoning as URTC's own Flash_WriteVerified, just 8 bytes
// at a time instead of 2 since this chip's flash controller only accepts
// FLASH_TYPEPROGRAM_DOUBLEWORD.
uint8_t Flash_WriteVerified(uint32_t addr, const uint8_t *buf, uint32_t len) {
    HAL_FLASH_Unlock();
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
    for (uint32_t i = 0; i < len; i += 8) {
        uint64_t dword = 0xFFFFFFFFFFFFFFFFULL; // erased-flash value pads any trailing partial double-word
        uint32_t chunk = (len - i < 8) ? (len - i) : 8;
        memcpy(&dword, &buf[i], chunk);
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr + i, dword) != HAL_OK) {
            HAL_FLASH_Lock();
            return 0;
        }
        if ((i & 0x3FF) == 0) HAL_IWDG_Refresh(&hiwdg); // every 1KB - cheap insurance, same spirit as URTC's own per-256-byte refresh
    }
    HAL_FLASH_Lock();
    __DSB(); // ensures the last flash write is fully complete before the read-back below trusts it

    for (uint32_t i = 0; i < len; i += 8) {
        uint64_t expected = 0xFFFFFFFFFFFFFFFFULL;
        uint32_t chunk = (len - i < 8) ? (len - i) : 8;
        memcpy(&expected, &buf[i], chunk);
        uint64_t actual = *(volatile uint64_t*)(addr + i);
        if (actual != expected) return 0;
        if ((i & 0x3FF) == 0) HAL_IWDG_Refresh(&hiwdg);
    }
    return 1;
}

// Copies one flash region into another, page by page: erase destination
// page, write+verify from source, refresh IWDG, repeat. Used for the
// backup-slot -> main-slot copy once backup has fully passed verification.
// Safe to call again from scratch after an interruption - source is never
// modified by this function, only destination.
uint8_t Flash_CopyRegion(uint32_t dest_addr, uint32_t src_addr, uint32_t total_len) {
    uint32_t num_pages = (total_len + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE;
    for (uint32_t p = 0; p < num_pages; p++) {
        uint32_t page_dest = dest_addr + (p * FLASH_PAGE_SIZE);
        uint32_t page_src  = src_addr  + (p * FLASH_PAGE_SIZE);
        uint32_t this_page_len = FLASH_PAGE_SIZE;
        if ((p + 1) * FLASH_PAGE_SIZE > total_len) {
            this_page_len = total_len - (p * FLASH_PAGE_SIZE);
        }

        FLASH_EraseInitTypeDef eraseInit;
        uint32_t pageError;
        eraseInit.TypeErase = FLASH_TYPEERASE_PAGES;
        eraseInit.Banks = FLASH_BANK_1;
        eraseInit.Page = AddrToPage(page_dest);
        eraseInit.NbPages = 1;
        HAL_FLASH_Unlock();
        __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
        HAL_StatusTypeDef res = HAL_FLASHEx_Erase(&eraseInit, &pageError);
        HAL_FLASH_Lock();
        if (res != HAL_OK || pageError != 0xFFFFFFFF) return 0;

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

// Writes without erasing first - callers are responsible for that (this
// chip's flash requires an erased page before programming, same rule as
// F3). Static and single-caller (Metadata_EraseAndWrite, right below).
static uint8_t Metadata_Write(const FirmwareMetadata_t *meta) {
    return Flash_WriteVerified(METADATA_ADDR, (const uint8_t*)meta, sizeof(FirmwareMetadata_t));
}

uint8_t Metadata_EraseAndWrite(const FirmwareMetadata_t *meta) {
    FirmwareMetadata_t current;
    if (Metadata_Read(&current) && memcmp(&current, meta, sizeof(FirmwareMetadata_t)) == 0) {
        return 1; // already holds exactly this content - skip the erase/write cycle
    }
    FLASH_EraseInitTypeDef eraseInit;
    uint32_t pageError;
    eraseInit.TypeErase = FLASH_TYPEERASE_PAGES;
    eraseInit.Banks = FLASH_BANK_1;
    eraseInit.Page = AddrToPage(METADATA_ADDR);
    eraseInit.NbPages = 1;
    HAL_FLASH_Unlock();
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
    HAL_StatusTypeDef res = HAL_FLASHEx_Erase(&eraseInit, &pageError);
    HAL_FLASH_Lock();
    if (res != HAL_OK || pageError != 0xFFFFFFFF) return 0;
    return Metadata_Write(meta);
}
