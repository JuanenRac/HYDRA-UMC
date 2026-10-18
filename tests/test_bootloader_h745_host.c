/* =============================================================================
 * HYDRA-UMC Firmware - Host logic tests for the H745 bootloaders' sector
 * erase, CRC32 and HMAC-SHA256
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Compiled once for the CM4 bootloader (Bank 2) and once for the CM7 bootloader
 * (Bank 1), each against its own bootloader_common.h, bootloader_crypto.c and
 * bootloader_flash.c (included below). The H7 erases whole 128 KB sectors, so
 * what matters is that a range of 2 KB protocol pages becomes exactly the
 * sectors it touches, in the right bank, one erase call and one watchdog
 * refresh per sector, and that a failure stops the erase.
 *
 * The program/read-back path is not run here: the H7 flash-word program call
 * takes the address of a 32-byte buffer as a 32-bit value, which a 64-bit host
 * cannot honour.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__) && defined(__x86_64__)
#include <sys/mman.h>
#define HAVE_LOW_BUFFERS 1
#endif

#include "stm32h7xx_hal.h"

#if defined(HYDRA_TEST_CM4)
#define EXPECTED_BANK       FLASH_BANK_2
#define SECTOR_BASE         0x08100000UL
#define TARGET_NAME         "CM4 (bank 2)"
#elif defined(HYDRA_TEST_CM7)
#define EXPECTED_BANK       FLASH_BANK_1
#define SECTOR_BASE         0x08000000UL
#define TARGET_NAME         "CM7 (bank 1)"
#else
#error "define HYDRA_TEST_CM4 or HYDRA_TEST_CM7"
#endif

static int failures;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("  FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
            failures++; \
        } \
    } while (0)

SPI_HandleTypeDef hspi1;
FDCAN_HandleTypeDef hfdcan1;
IWDG_HandleTypeDef hiwdg;

static int iwdg_refreshes, unlocks, locks, erase_calls, fail_erase_on_call;
static uint32_t erase_bank[64], erase_sector[64];

HAL_StatusTypeDef HAL_FLASH_Unlock(void) { unlocks++; return HAL_OK; }
HAL_StatusTypeDef HAL_FLASH_Lock(void) { locks++; return HAL_OK; }
HAL_StatusTypeDef HAL_IWDG_Refresh(IWDG_HandleTypeDef *h) { (void)h; iwdg_refreshes++; return HAL_OK; }
HAL_StatusTypeDef HAL_FLASH_Program(uint32_t t, uint32_t a, uint32_t d) { (void)t; (void)a; (void)d; return HAL_ERROR; }

void CAN_SendHeartbeat(uint8_t status, uint8_t progress_percent) { (void)status; (void)progress_percent; }

HAL_StatusTypeDef HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef *init, uint32_t *sector_error)
{
    erase_calls++;
    if (erase_calls <= 64) {
        erase_bank[erase_calls - 1] = init->Banks;
        erase_sector[erase_calls - 1] = init->Sector;
    }
    *sector_error = 0xFFFFFFFFUL;
    if (init->TypeErase != FLASH_TYPEERASE_SECTORS || init->NbSectors != 1U || init->VoltageRange != FLASH_VOLTAGE_RANGE_3) {
        *sector_error = init->Sector;
        return HAL_ERROR;
    }
    if ((fail_erase_on_call && erase_calls == fail_erase_on_call) || init->Sector > 7U) {
        *sector_error = init->Sector;
        return HAL_ERROR;
    }
    return HAL_OK;
}

#include "bootloader_crypto.c"
#include "bootloader_flash.c"

static void reset_model(void)
{
    iwdg_refreshes = unlocks = locks = erase_calls = fail_erase_on_call = 0;
}

static int hex_equals(const uint8_t *bytes, const char *hex)
{
    char text[65];
    for (int i = 0; i < 32; i++) sprintf(&text[i * 2], "%02x", bytes[i]);
    return strcmp(text, hex) == 0;
}

static void test_page_ranges_become_the_sectors_they_touch(void)
{
    printf("  Flash_ErasePages: 2 KB pages to 128 KB sectors\n");
    reset_model();
    CHECK(Flash_ErasePages(MAIN_APP_ADDR, APP_MAX_SIZE / FLASH_PAGE_SIZE) == 1, "the main application area");
    CHECK(erase_calls == 3, "384 KB are three sectors");
    CHECK(erase_bank[0] == EXPECTED_BANK && erase_bank[2] == EXPECTED_BANK, "in this core's bank");
    CHECK(erase_sector[0] == 2 && erase_sector[1] == 3 && erase_sector[2] == 4, "sectors 2, 3 and 4");
    CHECK(iwdg_refreshes == 3, "one watchdog refresh per sector");
    CHECK(unlocks == 3 && locks == 3, "unlocked and locked around every sector");

    reset_model();
    CHECK(Flash_ErasePages(BACKUP_APP_ADDR, APP_MAX_SIZE / FLASH_PAGE_SIZE) == 1, "the backup area");
    CHECK(erase_calls == 3 && erase_sector[0] == 5 && erase_sector[1] == 6 && erase_sector[2] == 7, "sectors 5, 6 and 7");

    reset_model();
    CHECK(Flash_ErasePages(BOOT_SECTOR_ADDR + 0, 1) == 1 && erase_sector[0] == 0, "an address in sector 0");
}

static void test_a_sector_is_erased_exactly_when_a_page_reaches_into_it(void)
{
    printf("  Flash_ErasePages: sector boundaries\n");
    reset_model();
    CHECK(Flash_ErasePages(MAIN_APP_ADDR, 64) == 1, "64 pages are exactly one sector");
    CHECK(erase_calls == 1, "one sector, not two");

    reset_model();
    CHECK(Flash_ErasePages(MAIN_APP_ADDR, 65) == 1, "65 pages");
    CHECK(erase_calls == 2, "reach into the next sector");

    reset_model();
    CHECK(Flash_ErasePages(MAIN_APP_ADDR, 1) == 1, "one page");
    CHECK(erase_calls == 1 && erase_sector[0] == 2, "only its own sector");

    reset_model();
    CHECK(EraseSectorsForRange(MAIN_APP_ADDR + FLASH_SECTOR_SIZE - 1, 2) == 1, "two bytes across a sector boundary");
    CHECK(erase_calls == 2 && erase_sector[0] == 2 && erase_sector[1] == 3, "erase both sectors");

    reset_model();
    CHECK(EraseSectorsForRange(MAIN_APP_ADDR, 1) == 1, "a single byte");
    CHECK(erase_calls == 1, "erases one sector");

    reset_model();
    CHECK(EraseSectorsForRange(METADATA_ADDR, 1) == 1, "the metadata record");
    CHECK(erase_calls == 1 && erase_sector[0] == 1, "has its own sector, number 1");
}

static void test_a_failed_sector_stops_the_erase(void)
{
    printf("  Flash_ErasePages: failure\n");
    reset_model();
    fail_erase_on_call = 2;
    CHECK(Flash_ErasePages(MAIN_APP_ADDR, APP_MAX_SIZE / FLASH_PAGE_SIZE) == 0, "second sector fails");
    CHECK(erase_calls == 2, "the third sector is not attempted");
    CHECK(unlocks == locks, "the flash is locked again");
    CHECK(iwdg_refreshes == 1, "refreshed only for the sector that was erased");
}

static void test_hmac_uses_the_same_algorithm_as_the_other_bootloaders(void)
{
    printf("  HMAC-SHA256 on this bootloader's copy\n");
#ifdef HAVE_LOW_BUFFERS
    /* the region is read through a 32-bit address, so the data goes in the low 2 GB */
    uint8_t *low = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    CHECK(low != MAP_FAILED, "a low buffer");
    if (low == MAP_FAILED) return;
    uint8_t out[32];
    uint8_t key[131];

    memset(key, 0x0b, 20);
    memcpy(low, "Hi There", 8);
    hmac_sha256_flash_region(key, 20, (uint32_t)(uintptr_t)low, 8, out);
    CHECK(hex_equals(out, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"), "RFC 4231 test case 1");

    memcpy(low, "what do ya want for nothing?", 28);
    hmac_sha256_flash_region((const uint8_t *)"Jefe", 4, (uint32_t)(uintptr_t)low, 28, out);
    CHECK(hex_equals(out, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"), "RFC 4231 test case 2");

    memset(key, 0xaa, 131);
    memcpy(low, "Test Using Larger Than Block-Size Key - Hash Key First", 54);
    hmac_sha256_flash_region(key, 131, (uint32_t)(uintptr_t)low, 54, out);
    CHECK(hex_equals(out, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"), "RFC 4231 test case 6 (key longer than the block)");

    iwdg_refreshes = 0;
    memset(low, 0, 4096);
    hmac_sha256_flash_region(key, 32, (uint32_t)(uintptr_t)low, 4096, out);
    CHECK(iwdg_refreshes >= 1, "the watchdog is fed while a region is hashed");
    munmap(low, 4096);
#else
    printf("    (skipped: needs Linux on x86-64)\n");
#endif
    uint8_t a[32], b[32];
    memset(a, 0x33, 32);
    memcpy(b, a, 32);
    CHECK(hmac_constant_time_compare(a, b, 32) == 1, "equal digests compare equal");
    b[31] ^= 1;
    CHECK(hmac_constant_time_compare(a, b, 32) == 0, "a difference in the last byte is seen");
    b[31] ^= 1;
    b[0] ^= 0x80;
    CHECK(hmac_constant_time_compare(a, b, 32) == 0, "and in the first");
}

static void test_crc32(void)
{
    printf("  CRC32 on this bootloader's copy\n");
    uint32_t crc = CRC32_Finalize(CRC32_Update(0xFFFFFFFFUL, (const uint8_t *)"123456789", 9));
    CHECK(crc == 0xCBF43926UL, "the standard check value");
    CHECK(CRC32_Finalize(0xFFFFFFFFUL) == 0UL, "empty input");
}

int main(void)
{
    printf("HYDRA-UMC host logic tests - H745 %s bootloader\n", TARGET_NAME);
    test_page_ranges_become_the_sectors_they_touch();
    test_a_sector_is_erased_exactly_when_a_page_reaches_into_it();
    test_a_failed_sector_stops_the_erase();
    test_hmac_uses_the_same_algorithm_as_the_other_bootloaders();
    test_crc32();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all H745 bootloader host tests passed\n");
    return 0;
}
