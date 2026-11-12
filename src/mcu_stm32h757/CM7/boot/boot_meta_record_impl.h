/*
 * =============================================================================
 * boot_meta_record_impl.h - the checks of one metadata record: its CRC, whether the slot is blank, whether it is valid
 * PROJECT: HYDRA-UMC
 * AUTHOR: JuanenRac (Electro Hobby 3D) - electrohobby3d@gmail.com
 * LICENSE: GPL-3.0-or-later - see repo root LICENSE
 *
 * One copy for the three bootloaders (firmware_base/boot, kept identical in each chip directory by tools/sync_firmware_base.py). It is included by
 * bootloader_flash.c, not compiled on its own, so the three flash files keep the same order of declarations and no build file changes.
 * =============================================================================
 */
#ifndef BOOT_META_RECORD_IMPL_H
#define BOOT_META_RECORD_IMPL_H

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

#endif /* BOOT_META_RECORD_IMPL_H */
