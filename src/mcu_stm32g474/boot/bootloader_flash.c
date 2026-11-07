// =============================================================================
// HYDRA-UMC Robot Controller Board Bootloader - CRC32, flash program/erase,
// and metadata persistence
// Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
// GPL-3.0-or-later - see repo root LICENSE
//
// Ported from URTC's own proven bootloader_flash.c, re-targeted for this
// chip's real flash controller differences from the F303's:
//   - STM32G4 programs in 64-bit DOUBLE-WORDS (FLASH_TYPEPROGRAM_DOUBLEWORD),
//     not 16-bit half-words - every write address here must be 8-byte
//     aligned, which FLASH_PAGE_SIZE (2048) and every call site's own
//     page-granular addressing already guarantee.
//   - STM32G4's FLASH_EraseInitTypeDef erases by BANK and PAGE NUMBER, not
//     by page ADDRESS the way F3's HAL takes it. This 512KB part is two
//     banks of 128 pages of 2KB (option bit DBANK = 1, the factory default),
//     numbered per bank - flash_geometry.h splits an address into
//     (bank, page) once, in one place.
//   - Requires DBANK = 1: in single-bank mode the pages are 4KB and the
//     numbering is different, so every erase here refuses to run (and the
//     update ends in an error) rather than erase the wrong pages.
// =============================================================================
#include "stm32g4xx_hal.h"
#include <string.h>
#include "bootloader_common.h"
#include "bootloader_flash.h"
#include "flash_geometry.h"
#include "flash_layer.h"      // the common checks of the three bootloaders (behind HYDRA_FLASH_LAYER, off by default)

#ifndef HYDRA_FLASH_LAYER
#define HYDRA_FLASH_LAYER 0
#endif

#include "boot_crc32_impl.h"

// -----------------------------------------------------------------------
// Flash routines
// -----------------------------------------------------------------------

// Erases the one 2KB page that starts at addr. Refuses (returns 0) outside
// the flash, or when the option bytes are not in dual-bank mode, where this
// file's geometry does not apply.
static uint8_t ErasePageAt(uint32_t addr) {
    uint8_t bank2;
    uint32_t page;
    if ((FLASH->OPTR & FLASH_OPTR_DBANK) == 0U) return 0;
    if (!FlashGeometry_AddrToBankPage(addr, &bank2, &page)) return 0;

    FLASH_EraseInitTypeDef eraseInit;
    uint32_t pageError;
    eraseInit.TypeErase = FLASH_TYPEERASE_PAGES;
    eraseInit.Banks = bank2 ? FLASH_BANK_2 : FLASH_BANK_1;
    eraseInit.Page = page;
    eraseInit.NbPages = 1;

    HAL_FLASH_Unlock();
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
    HAL_StatusTypeDef res = HAL_FLASHEx_Erase(&eraseInit, &pageError);
    HAL_FLASH_Lock();
    return res == HAL_OK && pageError == 0xFFFFFFFF;
}

#if HYDRA_FLASH_LAYER
// The three hardware calls under flash_layer.h: erase pages, program double-words, read. The range, alignment and blank checks and the read-back
// compare are the layer's.
static int g4_layer_erase(void *context, uint32_t address, uint32_t length) {
    (void)context;
    for (uint32_t done = 0; done < length; done += FLASH_PAGE_SIZE) {
        if (!ErasePageAt(address + done)) return 1;
        HAL_IWDG_Refresh(&hiwdg);
    }
    return 0;
}

static int g4_layer_program(void *context, uint32_t address, const uint8_t *data, uint32_t length) {
    (void)context;
    for (uint32_t i = 0; i < length; i += 8) {
        uint64_t dword;
        memcpy(&dword, &data[i], 8);
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, address + i, dword) != HAL_OK) return 1;
    }
    __DSB(); // the last flash write is complete before the read-back trusts it
    if ((address & 0x3FF) == 0) HAL_IWDG_Refresh(&hiwdg);
    return 0;
}

static int g4_layer_read(void *context, uint32_t address, uint8_t *out, uint32_t length) {
    (void)context;
    const volatile uint8_t *from = (const volatile uint8_t*)(uintptr_t)address;
    for (uint32_t i = 0; i < length; i++) out[i] = from[i];
    if ((address & 0x3FF) == 0) HAL_IWDG_Refresh(&hiwdg);
    return 0;
}

static const FlashLayerOps_t *g4_layer_ops(void) {
    static FlashLayerOps_t ops;
    ops.geometry = FlashLayer_GeometryG474();
    ops.context = 0;
    ops.erase = g4_layer_erase;
    ops.program = g4_layer_program;
    ops.read = g4_layer_read;
    return &ops;
}

uint8_t Flash_ErasePages(uint32_t start_addr, uint32_t num_pages) {
    if (num_pages == 0) return 1;
    if ((FLASH->OPTR & FLASH_OPTR_DBANK) == 0U) return 0; // single-bank mode: the page numbering of flash_geometry.h does not apply
    return FlashLayer_Erase(g4_layer_ops(), start_addr, num_pages * FLASH_PAGE_SIZE) == FLASH_LAYER_OK;
}
#else
uint8_t Flash_ErasePages(uint32_t start_addr, uint32_t num_pages) {
    // One page at a time with an IWDG refresh between each - same reasoning
    // as URTC's own bootloader: a single HAL_FLASHEx_Erase call spanning
    // every page at once loops internally with nothing able to refresh the
    // watchdog until it fully returns, and 236KB (118 pages) could plausibly
    // run long enough on its own to trip it.
    for (uint32_t i = 0; i < num_pages; i++) {
        if (!ErasePageAt(start_addr + i * FLASH_PAGE_SIZE)) return 0;
        HAL_IWDG_Refresh(&hiwdg);
    }
    return 1;
}

#endif

// Writes len bytes (rounded up to a double-word, i.e. 8 bytes) starting at
// addr, then reads every double-word back and compares it against the
// source buffer before reporting success - same "catch a stuck flash cell
// at write time" reasoning as URTC's own Flash_WriteVerified, just 8 bytes
// at a time instead of 2 since this chip's flash controller only accepts
// FLASH_TYPEPROGRAM_DOUBLEWORD.
#if HYDRA_FLASH_LAYER
uint8_t Flash_WriteVerified(uint32_t addr, const uint8_t *buf, uint32_t len) {
    if (len == 0) return 1;
    HAL_FLASH_Unlock();
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
    const FlashLayerStatus status = FlashLayer_Write(g4_layer_ops(), addr, buf, len);
    HAL_FLASH_Lock();
    return status == FLASH_LAYER_OK;
}
#else
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
#endif

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

#if HYDRA_FLASH_LAYER
        if (!Flash_ErasePages(page_dest, 1)) return 0;
#else
        if (!ErasePageAt(page_dest)) return 0;
#endif

        if (!Flash_WriteVerified(page_dest, (const uint8_t*)page_src, this_page_len)) return 0;
        HAL_IWDG_Refresh(&hiwdg);

        uint8_t pct = (uint8_t)(((uint64_t)(p + 1) * 100) / num_pages);
        CAN_SendHeartbeat(STATUS_COPYING, pct);
    }
    return 1;
}

// ---- metadata: an append-only log (see MetaRecord_t in bootloader_common.h) ------------------------------------------------------------------
_Static_assert(sizeof(MetaRecord_t) == META_RECORD_SIZE, "a metadata record has the size the layout was made for");

#include "boot_meta_record_impl.h"

static const MetaRecord_t *meta_slot(uint32_t page, uint32_t index) {
    return (const MetaRecord_t*)(METADATA_ADDR + page * FLASH_PAGE_SIZE + index * META_RECORD_SIZE);
}

// The valid record with the highest generation, over the two pages of the log; 0 when there is none. A record cut by a power loss fails its CRC.
static uint8_t meta_find_newest(uint32_t *page, uint32_t *index) {
    uint8_t found = 0;
    uint32_t best_generation = 0;
    for (uint32_t p = 0; p < 2u; p++) {
        for (uint32_t i = 0; i < META_RECORD_SLOTS; i++) {
            const MetaRecord_t *record = meta_slot(p, i);
            if (!meta_slot_valid(record)) continue;
            if (!found || record->generation > best_generation) {
                found = 1;
                best_generation = record->generation;
                *page = p;
                *index = i;
            }
        }
    }
    return found;
}

uint8_t Metadata_Read(FirmwareMetadata_t *out) {
    uint32_t page = 0, index = 0;
    if (!meta_find_newest(&page, &index)) {
        memset(out, 0, sizeof(*out));
        return 0;
    }
    memcpy(out, &meta_slot(page, index)->meta, sizeof(*out));
    return 1;
}

// Appends the state as a new record. The log has two pages: when the one in use is full, the other one (which only holds older records) is erased and
// takes the new record, so the newest state is never without a valid copy: a cut during that erase or write leaves the full page and its newest
// record in force. The name is kept from the single-record page this replaced.
uint8_t Metadata_EraseAndWrite(const FirmwareMetadata_t *meta) {
    uint32_t page = 0, index = 0;
    uint32_t generation = 1;
    uint32_t target_page = 0;
    uint8_t have = meta_find_newest(&page, &index);
    if (have) {
        const MetaRecord_t *latest = meta_slot(page, index);
        // Skip the write entirely if the log already ends with exactly this state - the page has the same endurance as any other.
        if (memcmp(&latest->meta, meta, sizeof(*meta)) == 0) return 1;
        generation = latest->generation + 1u;
        target_page = page;
    }
    uint32_t slot = META_RECORD_SLOTS;
    for (uint32_t i = 0; i < META_RECORD_SLOTS; i++) {
        if (meta_slot_blank(meta_slot(target_page, i))) {
            slot = i;
            break;
        }
    }
    if (slot == META_RECORD_SLOTS) {
        target_page = have ? 1u - page : 0u;
        if (!ErasePageAt((uint32_t)(METADATA_ADDR + target_page * FLASH_PAGE_SIZE))) return 0;
        slot = 0;
    }
    MetaRecord_t record;
    memset(&record, 0, sizeof(record));
    record.generation = generation;
    record.meta = *meta;
    record.crc = meta_record_crc(&record);
    return Flash_WriteVerified((uint32_t)(METADATA_ADDR + target_page * FLASH_PAGE_SIZE + slot * META_RECORD_SIZE), (const uint8_t*)&record, META_RECORD_SIZE);
}
