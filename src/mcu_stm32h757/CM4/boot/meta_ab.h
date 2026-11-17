/*
 * =============================================================================
 * meta_ab.h - the metadata log in two sectors that take turns (HYDRA_META_AB)
 * PROJECT: HYDRA-UMC
 * AUTHOR: JuanenRac (Electro Hobby 3D) - electrohobby3d@gmail.com
 * LICENSE: GPL-3.0-or-later - see repo root LICENSE
 *
 * Included by bootloader_flash.c in place of the single-sector log, only when HYDRA_META_AB is 1 (it is 0 as it ships, and then this file is not read
 * and the binary does not change). Region A is the metadata sector the single-sector log has always used (METADATA_ADDR); region B is a second sector
 * (METADATA_B_ADDR). Both hold the same records (MetaRecord_t), and the newest valid record of the two is the state.
 *
 * - A log is appended to its sector until it is full. Nothing about this changes for a board that has only region A: its records stay where they are
 *   and its log goes on in the next free slot, so a board needs no migration step and keeps its serial and its rollback counter.
 * - When the sector that holds the newest record is full, the OTHER sector is erased (it holds only older records) and the new record is the first
 *   of it. The sector that was full is not touched, so it still holds the newest state until the new record is complete: a power cut at any point of
 *   the erase or of the write leaves the previous state readable, with the rollback counter it had. A record that was cut fails its CRC and is ignored.
 * - A region that is already blank is not erased again (the retry after a cut costs no erase cycle).
 * - The sector of region B must not be used for anything else: the layout with HYDRA_META_AB gives up the last sector of the application backup slot
 *   (see bootloader_common.h; the longest application is then 256 KB).
 * =============================================================================
 */
#ifndef META_AB_H
#define META_AB_H

static const MetaRecord_t *meta_slot_in(uint32_t base, uint32_t index) {
    return (const MetaRecord_t*)(base + index * META_RECORD_SIZE);
}

// Region A, the sector of the single-sector log.
static const MetaRecord_t *meta_slot(uint32_t index) {
    return meta_slot_in(METADATA_ADDR, index);
}

// The valid record with the highest generation of one region, or -1. A record cut by a power loss fails its CRC.
static int32_t meta_region_newest(uint32_t base, uint32_t *generation_out) {
    int32_t best = -1;
    uint32_t best_generation = 0;
    for (uint32_t i = 0; i < META_RECORD_SLOTS; i++) {
        const MetaRecord_t *record = meta_slot_in(base, i);
        if (!meta_slot_valid(record)) continue;
        if (best < 0 || record->generation > best_generation) {
            best = (int32_t)i;
            best_generation = record->generation;
        }
    }
    *generation_out = best_generation;
    return best;
}

// The first slot of a region that is blank, or META_RECORD_SLOTS when there is none.
static uint32_t meta_region_first_blank(uint32_t base) {
    for (uint32_t i = 0; i < META_RECORD_SLOTS; i++) {
        if (meta_slot_blank(meta_slot_in(base, i))) return i;
    }
    return META_RECORD_SLOTS;
}

// 1 when every byte of the region's records is the erased value (the bytes after the last record are not looked at: nothing writes there).
static uint8_t meta_region_is_blank(uint32_t base) {
    for (uint32_t i = 0; i < META_RECORD_SLOTS; i++) {
        if (!meta_slot_blank(meta_slot_in(base, i))) return 0;
    }
    return 1;
}

// The region that holds the newest valid record of the two; returns 0 when there is no record at all. generation_out is its generation.
static uint8_t meta_find_newest_region(uint32_t *base_out, int32_t *index_out, uint32_t *generation_out) {
    uint32_t generation_a = 0, generation_b = 0;
    const int32_t newest_a = meta_region_newest(METADATA_ADDR, &generation_a);
    const int32_t newest_b = meta_region_newest(METADATA_B_ADDR, &generation_b);
    if (newest_a < 0 && newest_b < 0) return 0;
    if (newest_b < 0 || (newest_a >= 0 && generation_a >= generation_b)) {
        *base_out = METADATA_ADDR;
        *index_out = newest_a;
        *generation_out = generation_a;
    } else {
        *base_out = METADATA_B_ADDR;
        *index_out = newest_b;
        *generation_out = generation_b;
    }
    return 1;
}

uint8_t Metadata_Read(FirmwareMetadata_t *out) {
    uint32_t base = 0, generation = 0;
    int32_t index = -1;
    if (!meta_find_newest_region(&base, &index, &generation)) {
        memset(out, 0, sizeof(*out));
        return 0;
    }
    memcpy(out, &meta_slot_in(base, (uint32_t)index)->meta, sizeof(*out));
    return 1;
}

// Appends the state as a new record. The name is kept from the single-sector layout. See the header of this file for what a power cut leaves.
uint8_t Metadata_EraseAndWrite(const FirmwareMetadata_t *meta) {
    uint32_t generation = 1;
    uint32_t base = METADATA_ADDR;
    uint32_t latest_generation = 0;
    int32_t latest_index = -1;
    if (meta_find_newest_region(&base, &latest_index, &latest_generation)) {
        const MetaRecord_t *latest = meta_slot_in(base, (uint32_t)latest_index);
        // Skip the write entirely if the log already ends with exactly this state - the sector has the same endurance as any other.
        if (memcmp(&latest->meta, meta, sizeof(*meta)) == 0) return 1;
        generation = latest_generation + 1u;
    }
    uint32_t slot = meta_region_first_blank(base);
    if (slot == META_RECORD_SLOTS) {
        // The sector with the newest record is full: the next record starts the other one, which holds only older records.
        base = (base == METADATA_ADDR) ? METADATA_B_ADDR : METADATA_ADDR;
        if (!meta_region_is_blank(base) && !EraseSectorsForRange(base, 1)) return 0;
        slot = 0;
    }
    MetaRecord_t record;
    memset(&record, 0, sizeof(record));
    record.generation = generation;
    record.meta = *meta;
    record.crc = meta_record_crc(&record);
    return Flash_WriteVerified((uint32_t)(base + slot * META_RECORD_SIZE), (const uint8_t*)&record, META_RECORD_SIZE);
}

#endif /* META_AB_H */
