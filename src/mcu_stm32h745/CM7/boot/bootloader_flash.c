// =============================================================================
// HYDRA-UMC Kinematic Brain Bootloader (CM7) - CRC32, flash program/erase,
// and metadata persistence
// Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
// GPL-3.0 - see repo root LICENSE
//
// NOT a simple port of the F3/G4 pattern (URTC's own bootloader_flash.c,
// HYDRA-UMC's own G474 port) - STM32H7's flash controller is different
// enough to need real redesign, not just a HAL-call substitution:
//   - Erases in 128 KB SECTORS (FLASH_TYPEERASE_SECTORS, 8 per 1MB bank),
//     not 2KB pages. The OTA protocol's own "page" concept (2048-byte
//     transfer chunks, PAGE_ACK-per-chunk) stays exactly as every other
//     tier uses it - FLASH_PAGE_SIZE here is a LOGICAL unit for that
//     protocol math only, NOT what gets erased. Flash_ErasePages() below
//     translates a logical page range into the whole sectors it spans and
//     erases each ONCE - erasing per logical page the way F3/G4 do would
//     erase the SAME 128KB sector ~64 times for its ~64 logical pages,
//     destroying every earlier page written into that sector before its
//     later neighbors ever got written.
//   - Programs in 256-bit (32-byte) FLASHWORDs (FLASH_TYPEPROGRAM_
//     FLASHWORD) - and unlike F3's HALFWORD/G4's DOUBLEWORD (where the
//     HAL_FLASH_Program() "Data" argument IS the value to write), H7's
//     FLASHWORD variant takes a POINTER to 8 words of source data instead
//     - the value itself doesn't fit in a single function argument.
//   - A single HAL_FLASHEx_Erase() call for MULTIPLE sectors blocks
//     internally for the whole operation with no chance to refresh the
//     IWDG until it returns - same reasoning F3/G4's bootloaders already
//     apply at page granularity, just scaled up: this erases ONE sector at
//     a time with a refresh between each, never more.
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

// -----------------------------------------------------------------------
// Sector-aware erase - the real difference from every other bootloader in
// this project. Erases every 128KB sector touched by [start_addr,
// start_addr+total_len), one sector at a time, each followed by an IWDG
// refresh. Bank 1 only (this is the CM7 core's own bank) - see the CM4
// bootloader's own bootloader_flash.c for the Bank 2 equivalent.
// -----------------------------------------------------------------------
static uint8_t EraseSectorsForRange(uint32_t start_addr, uint32_t total_len) {
    uint32_t end_addr = start_addr + total_len;
    uint32_t first_sector = (start_addr - BANK1_BASE) / FLASH_SECTOR_SIZE;
    uint32_t last_sector = (end_addr - 1 - BANK1_BASE) / FLASH_SECTOR_SIZE;

    for (uint32_t s = first_sector; s <= last_sector; s++) {
        FLASH_EraseInitTypeDef eraseInit;
        uint32_t sectorError;
        eraseInit.TypeErase = FLASH_TYPEERASE_SECTORS;
        eraseInit.Banks = FLASH_BANK_1;
        eraseInit.Sector = s;
        eraseInit.NbSectors = 1;
        eraseInit.VoltageRange = FLASH_VOLTAGE_RANGE_3; // assumes 2.7-3.6V supply - matches this project's 3.3V logic rail everywhere else

        HAL_FLASH_Unlock();
        HAL_StatusTypeDef res = HAL_FLASHEx_Erase(&eraseInit, &sectorError);
        HAL_FLASH_Lock();
        if (res != HAL_OK || sectorError != 0xFFFFFFFF) return 0;
        HAL_IWDG_Refresh(&hiwdg);
    }
    return 1;
}

// start_addr/num_pages are the same LOGICAL 2KB-page units every other
// tier's protocol uses (matches PAGE_ACK/heartbeat-percent math) -
// translated here into the sectors that range actually spans.
uint8_t Flash_ErasePages(uint32_t start_addr, uint32_t num_pages) {
    return EraseSectorsForRange(start_addr, num_pages * FLASH_PAGE_SIZE);
}

// Writes len bytes (rounded up to a 32-byte FLASHWORD) starting at addr,
// then reads every FLASHWORD back and compares it against the source
// buffer before reporting success - same "catch a stuck flash cell at
// write time" reasoning as every other bootloader in this project, 32
// bytes at a time instead of 2 or 8 since FLASH_TYPEPROGRAM_FLASHWORD is
// this chip's only programming granularity.
uint8_t Flash_WriteVerified(uint32_t addr, const uint8_t *buf, uint32_t len) {
    HAL_FLASH_Unlock();
    for (uint32_t i = 0; i < len; i += 32) {
        uint32_t word[8];
        memset(word, 0xFF, sizeof(word)); // erased-flash value pads any trailing partial flashword
        uint32_t chunk = (len - i < 32) ? (len - i) : 32;
        memcpy(word, &buf[i], chunk);
        // FLASHWORD's own HAL signature takes a POINTER to the 8 source
        // words (cast to uint32_t), not the data itself - real difference
        // from F3's HALFWORD/G4's DOUBLEWORD, both of which pass the value
        // directly.
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_FLASHWORD, addr + i, (uint32_t)(uintptr_t)word) != HAL_OK) {
            HAL_FLASH_Lock();
            return 0;
        }
        if ((i & 0xFFF) == 0) HAL_IWDG_Refresh(&hiwdg); // every 4KB
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

// Copies one flash region into another. Unlike F3/G4's per-page erase-then-
// write loop, this ERASES EVERY DESTINATION SECTOR UP FRONT (once each,
// via EraseSectorsForRange) and only then writes page by page with no
// further erasing - erasing per logical page at this chip's 128KB sector
// granularity would destroy earlier pages in the same sector before their
// neighbors were written, which is exactly the bug this structure avoids.
// Not safely resumable mid-way through the SAME sector if interrupted
// (a partially-copied sector's un-copied pages are gone, erased along with
// the rest of that sector) - ApplicationIsValid()'s own COPY_PENDING resume
// path calls this again from the top on the next boot either way, and
// source (backup slot) is never modified by this function, so a resume
// always has a complete, untouched source to redo the copy from.
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

// Erases metadata's own dedicated sector (sector 1 - see bootloader_
// common.h's own METADATA_ADDR comment for why this chip needs metadata to
// have a full sector to itself, unlike F3/G4's page-granular split): this
// bootloader's own code lives in sector 0, so this erase never touches the
// sector the CPU is actively executing from - the hazard a shared-sector
// design would have had (STM32H7 can stall/fault reading from a sector
// mid-erase) doesn't apply here.
uint8_t Metadata_EraseAndWrite(const FirmwareMetadata_t *meta) {
    FirmwareMetadata_t current;
    if (Metadata_Read(&current) && memcmp(&current, meta, sizeof(FirmwareMetadata_t)) == 0) {
        return 1;
    }
    if (!EraseSectorsForRange(METADATA_ADDR, 1)) return 0;
    return Metadata_Write(meta);
}
