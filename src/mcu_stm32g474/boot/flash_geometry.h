/* =============================================================================
 * HYDRA-UMC Robot Controller Board Bootloader - flash bank/page geometry
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see repo root LICENSE
 * =============================================================================
 * The STM32G474RE has 512 KB of flash. With the DBANK option bit set (the
 * factory default) it is two banks of 128 pages of 2 KB each: pages are
 * numbered per bank, 0-127, and the erase command needs the bank as well as
 * the page number. A flat 0-255 page number does not exist on this part (the
 * erase command only takes the low 7 bits of the page), so every flash
 * address has to be split into (bank, page) before it is erased.
 *
 * HAL-free so it can be tested on the host with plain gcc.
 */
#ifndef HYDRA_UMC_G474_FLASH_GEOMETRY_H
#define HYDRA_UMC_G474_FLASH_GEOMETRY_H

#include <stdint.h>

#define FLASH_GEOMETRY_BASE        0x08000000UL
#define FLASH_GEOMETRY_BANK_SIZE   0x00040000UL /* 256 KB per bank */
#define FLASH_GEOMETRY_PAGE_SIZE   2048UL

/* Splits a flash address into its bank (0 = bank 1, 1 = bank 2) and the page
 * number inside that bank. Returns 1 on success, 0 when the address is not
 * inside the 512 KB of flash. */
static inline uint8_t FlashGeometry_AddrToBankPage(uint32_t addr, uint8_t *bank2, uint32_t *page)
{
    if (addr < FLASH_GEOMETRY_BASE || addr >= FLASH_GEOMETRY_BASE + 2UL * FLASH_GEOMETRY_BANK_SIZE) {
        return 0;
    }
    uint32_t offset = addr - FLASH_GEOMETRY_BASE;
    *bank2 = (offset >= FLASH_GEOMETRY_BANK_SIZE) ? 1U : 0U;
    *page = (offset % FLASH_GEOMETRY_BANK_SIZE) / FLASH_GEOMETRY_PAGE_SIZE;
    return 1;
}

#endif /* HYDRA_UMC_G474_FLASH_GEOMETRY_H */
