/* =============================================================================
 * HYDRA-UMC Firmware - Host logic tests for the G474 flash bank/page split
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0 - see LICENSE
 * =============================================================================
 * The erase command of this part takes a bank and a 7-bit page number; the
 * old code passed a flat 0-255 page number and so erased the wrong pages for
 * everything above 256 KB (the backup slot).
 */
#include <stdio.h>

#include "flash_geometry.h"

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("  FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
            failures++; \
        } \
    } while (0)

static int failures;

static void expect(uint32_t addr, uint8_t want_bank2, uint32_t want_page, const char *what)
{
    uint8_t bank2 = 9;
    uint32_t page = 9999;
    TEST_ASSERT(FlashGeometry_AddrToBankPage(addr, &bank2, &page) == 1, what);
    TEST_ASSERT(bank2 == want_bank2, what);
    TEST_ASSERT(page == want_page, what);
}

int main(void)
{
    printf("HYDRA-UMC host logic tests - flash_geometry.h\n");

    expect(0x08000000UL, 0, 0, "first page of bank 1");
    expect(0x08008000UL, 0, 16, "metadata page");
    expect(0x08009000UL, 0, 18, "application slot start");
    expect(0x0803F800UL, 0, 127, "last page of bank 1");
    expect(0x08040000UL, 1, 0, "first page of bank 2");
    expect(0x08044000UL, 1, 8, "backup slot start (was erased as page 8 of bank 1 before)");
    expect(0x0807F800UL, 1, 127, "last page of bank 2");

    /* every page of the backup slot maps into bank 2 */
    for (uint32_t addr = 0x08044000UL; addr < 0x08044000UL + 236UL * 1024UL; addr += FLASH_GEOMETRY_PAGE_SIZE) {
        uint8_t bank2;
        uint32_t page;
        TEST_ASSERT(FlashGeometry_AddrToBankPage(addr, &bank2, &page) == 1 && bank2 == 1 && page < 128, "backup slot stays inside bank 2");
    }

    uint8_t bank2;
    uint32_t page;
    TEST_ASSERT(FlashGeometry_AddrToBankPage(0x07FFFFFFUL, &bank2, &page) == 0, "below flash is rejected");
    TEST_ASSERT(FlashGeometry_AddrToBankPage(0x08080000UL, &bank2, &page) == 0, "above flash is rejected");

    if (failures == 0) {
        printf("  OK   all flash_geometry host tests passed\n");
        return 0;
    }
    printf("  %d assertion(s) failed\n", failures);
    return 1;
}
