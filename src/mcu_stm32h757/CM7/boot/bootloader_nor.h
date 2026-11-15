// =============================================================================
// HYDRA-UMC Kinematic Brain Bootloader (CM7) - the NOR flash as staging area and backup of the 768 KB application slot
// Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
// GPL-3.0-or-later - see repo root LICENSE
//
// With the long slot (HYDRA_APP_SLOT_768K) the second half of bank 1 is the application, so an update is staged in the NOR flash of SPI5 and the copy of the application lives there too
// (Common/boot_nor_stage.h over Common/nor_backup_plan.h). This file is the part that touches the board: it starts SPI5 from the pins of config/nor_spi5.cfg (Common/nor_spi5_pins.h) the first
// time anything needs the NOR, gives the flash layer the three operations on the application slot, and keeps the watchdog fed while the slow NOR operations run. OFF by default: it compiles to
// nothing unless HYDRA_NOR_BACKUP is 1 (and then HYDRA_NOR_SPI5 must be 1 as well). Not run on a board.
// =============================================================================
#ifndef BOOTLOADER_NOR_H
#define BOOTLOADER_NOR_H

#include <stdint.h>

#ifndef HYDRA_NOR_BACKUP
#define HYDRA_NOR_BACKUP 0
#endif

#if HYDRA_NOR_BACKUP

#ifndef HYDRA_NOR_SPI5
#define HYDRA_NOR_SPI5 0
#endif
#if !HYDRA_NOR_SPI5
#error "HYDRA_NOR_BACKUP needs HYDRA_NOR_SPI5=1: the copy is read and written through the SPI5 driver"
#endif

// All of these answer 1 on success and 0 on any failure (the same convention as bootloader_flash.h); a NOR that does not answer makes every one of them 0.
uint8_t BootNor_Begin(uint32_t total);                                                                // erase the image area for an update of `total` bytes
uint8_t BootNor_Page(uint32_t index, const uint8_t *data, uint32_t length);                            // page `index` of FLASH_PAGE_SIZE bytes
uint32_t BootNor_Word(uint32_t offset);                                                               // the word at `offset` of the staged image, 0 when it cannot be read (an image cannot start with a stack pointer of 0)
uint8_t BootNor_Digest(const uint8_t *prefix, uint32_t prefix_len, uint32_t total, uint8_t out[32]);  // SHA-256 of prefix + the staged image, as sha256_prefixed_flash_region does for flash; zero in `out` when it cannot be read
uint8_t BootNor_Finish(uint32_t total, uint32_t crc32);                                               // check the CRC-32 of the staged image and write its header
uint8_t BootNor_Commit(uint32_t total, uint32_t crc32);                                               // write the staged image over the application slot and read it back
// Boot-time rule: an application that is not good is replaced by the copy when the copy is the image the metadata describes. Returns 1 when the application was restored.
uint8_t BootNor_RestoreIfLost(uint32_t size, uint32_t crc32);

#endif // HYDRA_NOR_BACKUP

#endif // BOOTLOADER_NOR_H
