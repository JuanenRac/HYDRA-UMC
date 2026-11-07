// =============================================================================
// HYDRA-UMC Kinematic Brain Bootloader (CM7) - CRC32, flash program/erase,
// and metadata persistence
// Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
// GPL-3.0-or-later - see repo root LICENSE
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
#include "flash_layer.h"      // the common checks of the three bootloaders (behind HYDRA_FLASH_LAYER, off by default)

#ifndef HYDRA_FLASH_LAYER
#define HYDRA_FLASH_LAYER 0
#endif

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
#if HYDRA_FLASH_LAYER
// The geometry of this core's bank under flash_layer.h: 1 MB, sectors of 128 KB, flash words of 32 bytes. The erase unit of the layer is the sector.
static const FlashLayerGeometry_t *this_bank_geometry(void) {
    static const FlashLayerGeometry_t g = {BANK1_BASE, 0x00100000UL, FLASH_SECTOR_SIZE, 32u};
    return &g;
}

// The hardware calls under the layer: erase whole sectors one at a time with a watchdog refresh between each, program flash words, read bytes.
static int h7_layer_erase(void *context, uint32_t address, uint32_t length) {
    (void)context;
    for (uint32_t done = 0; done < length; done += FLASH_SECTOR_SIZE) {
        FLASH_EraseInitTypeDef eraseInit;
        uint32_t sectorError;
        eraseInit.TypeErase = FLASH_TYPEERASE_SECTORS;
        eraseInit.Banks = FLASH_BANK_1;
        eraseInit.Sector = (address + done - BANK1_BASE) / FLASH_SECTOR_SIZE;
        eraseInit.NbSectors = 1;
        eraseInit.VoltageRange = FLASH_VOLTAGE_RANGE_3;
        HAL_FLASH_Unlock();
        HAL_StatusTypeDef res = HAL_FLASHEx_Erase(&eraseInit, &sectorError);
        HAL_FLASH_Lock();
        if (res != HAL_OK || sectorError != 0xFFFFFFFF) return 1;
        HAL_IWDG_Refresh(&hiwdg);
    }
    return 0;
}

static int h7_layer_program(void *context, uint32_t address, const uint8_t *data, uint32_t length) {
    (void)context;
    for (uint32_t i = 0; i < length; i += 32) {
        uint32_t word[8]; // the HAL takes the address of 8 source words
        memcpy(word, &data[i], 32);
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_FLASHWORD, address + i, (uint32_t)(uintptr_t)word) != HAL_OK) return 1;
    }
    __DSB(); // the last flash write is complete before the read-back trusts it
    if ((address & 0xFFF) == 0) HAL_IWDG_Refresh(&hiwdg);
    return 0;
}

static int h7_layer_read(void *context, uint32_t address, uint8_t *out, uint32_t length) {
    (void)context;
    const volatile uint8_t *from = (const volatile uint8_t*)(uintptr_t)address;
    for (uint32_t i = 0; i < length; i++) out[i] = from[i];
    if ((address & 0xFFF) == 0) HAL_IWDG_Refresh(&hiwdg);
    return 0;
}

static const FlashLayerOps_t *h7_layer_ops(void) {
    static FlashLayerOps_t ops;
    ops.geometry = this_bank_geometry();
    ops.context = 0;
    ops.erase = h7_layer_erase;
    ops.program = h7_layer_program;
    ops.read = h7_layer_read;
    return &ops;
}

// Erases every sector touched by [start_addr, start_addr + total_len), inside this core's bank only (a range outside it is refused).
static uint8_t EraseSectorsForRange(uint32_t start_addr, uint32_t total_len) {
    if (total_len == 0) return 1;
    if (!FlashLayer_InRange(this_bank_geometry(), start_addr, total_len)) return 0;
    const uint32_t first_sector = (start_addr - BANK1_BASE) / FLASH_SECTOR_SIZE;
    const uint32_t last_sector = (start_addr + total_len - 1 - BANK1_BASE) / FLASH_SECTOR_SIZE;
    return FlashLayer_Erase(h7_layer_ops(), BANK1_BASE + first_sector * FLASH_SECTOR_SIZE, (last_sector - first_sector + 1) * FLASH_SECTOR_SIZE) == FLASH_LAYER_OK;
}
#else
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
#endif

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
#if HYDRA_FLASH_LAYER
uint8_t Flash_WriteVerified(uint32_t addr, const uint8_t *buf, uint32_t len) {
    if (len == 0) return 1;
    HAL_FLASH_Unlock();
    const FlashLayerStatus status = FlashLayer_Write(h7_layer_ops(), addr, buf, len);
    HAL_FLASH_Lock();
    return status == FLASH_LAYER_OK;
}
#else
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
#endif

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

// ---- metadata: an append-only log (see MetaRecord_t in bootloader_common.h) ------------------------------------------------------------------
_Static_assert(sizeof(MetaRecord_t) == META_RECORD_SIZE, "a metadata record has the size the layout was made for");

static uint32_t meta_record_crc(const MetaRecord_t *record) {
    return CRC32_Finalize(CRC32_Update(0xFFFFFFFFUL, (const uint8_t*)record, META_RECORD_SIZE - 4u));
}

static uint8_t meta_slot_blank(const MetaRecord_t *record) {
    const uint8_t *bytes = (const uint8_t*)record;
    for (uint32_t i = 0; i < META_RECORD_SIZE; i++) {
        if (bytes[i] != 0xFF) return 0;
    }
    return 1;
}

static uint8_t meta_slot_valid(const MetaRecord_t *record) {
    return record->meta.magic == METADATA_MAGIC_VALID && record->generation != 0xFFFFFFFFUL && meta_record_crc(record) == record->crc;
}

static const MetaRecord_t *meta_slot(uint32_t index) {
    return (const MetaRecord_t*)(METADATA_ADDR + index * META_RECORD_SIZE);
}

// The valid record with the highest generation, or -1. A record cut by a power loss fails its CRC.
static int32_t meta_find_newest(void) {
    int32_t best = -1;
    uint32_t best_generation = 0;
    for (uint32_t i = 0; i < META_RECORD_SLOTS; i++) {
        const MetaRecord_t *record = meta_slot(i);
        if (!meta_slot_valid(record)) continue;
        if (best < 0 || record->generation > best_generation) {
            best = (int32_t)i;
            best_generation = record->generation;
        }
    }
    return best;
}

uint8_t Metadata_Read(FirmwareMetadata_t *out) {
    int32_t newest = meta_find_newest();
    if (newest < 0) {
        memset(out, 0, sizeof(*out));
        return 0;
    }
    memcpy(out, &meta_slot((uint32_t)newest)->meta, sizeof(*out));
    return 1;
}

// Appends the state as a new record; the sector is erased only when its records are all used (then the new record is the first of a fresh log). The
// name is kept from the single-record sector this replaced. A cut right at that erase loses the log: the board then starts as a first boot, with the
// floor of the rollback counter compiled into the bootloader.
uint8_t Metadata_EraseAndWrite(const FirmwareMetadata_t *meta) {
    uint32_t generation = 1;
    int32_t newest = meta_find_newest();
    if (newest >= 0) {
        const MetaRecord_t *latest = meta_slot((uint32_t)newest);
        // Skip the write entirely if the log already ends with exactly this state - the sector has the same endurance as any other.
        if (memcmp(&latest->meta, meta, sizeof(*meta)) == 0) return 1;
        generation = latest->generation + 1u;
    }
    uint32_t slot = META_RECORD_SLOTS;
    for (uint32_t i = 0; i < META_RECORD_SLOTS; i++) {
        if (meta_slot_blank(meta_slot(i))) {
            slot = i;
            break;
        }
    }
    if (slot == META_RECORD_SLOTS) {
        if (!EraseSectorsForRange(METADATA_ADDR, 1)) return 0;
        slot = 0;
    }
    MetaRecord_t record;
    memset(&record, 0, sizeof(record));
    record.generation = generation;
    record.meta = *meta;
    record.crc = meta_record_crc(&record);
    return Flash_WriteVerified((uint32_t)(METADATA_ADDR + slot * META_RECORD_SIZE), (const uint8_t*)&record, META_RECORD_SIZE);
}
