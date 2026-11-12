/*
 * =============================================================================
 * boot_crc32_impl.h - the software CRC32 of the bootloaders (standard polynomial, same variant on every chip)
 * PROJECT: HYDRA-UMC
 * AUTHOR: JuanenRac (Electro Hobby 3D) - electrohobby3d@gmail.com
 * LICENSE: GPL-3.0-or-later - see repo root LICENSE
 *
 * One copy for the three bootloaders (firmware_base/boot, kept identical in each chip directory by tools/sync_firmware_base.py). It is included by
 * bootloader_flash.c, not compiled on its own, so the three flash files keep the same order of declarations and no build file changes.
 * =============================================================================
 */
#ifndef BOOT_CRC32_IMPL_H
#define BOOT_CRC32_IMPL_H

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

#endif /* BOOT_CRC32_IMPL_H */
