/* =============================================================================
 * HYDRA-UMC Firmware - Host tests of the H745 bootloaders' flash writing, region copy and metadata log
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Compiled once for the CM4 bootloader (bank 2) and once for the CM7 bootloader (bank 1), each with and without the common flash layer
 * (HYDRA_FLASH_LAYER=1), against its own bootloader_flash.c. tests/test_bootloader_h745_host.c covers the sector erase, the CRC32 and the SHA-256 and
 * says why it cannot run the program path: the H7 flash-word call takes the ADDRESS of the 32 source bytes as a 32-bit value, which a pointer on a 64-bit
 * host does not fit. This file runs it all the same, by running the test on a stack that lives in the low 2 GB (makecontext on a MAP_32BIT mapping), so
 * that the address of the buffer on the stack survives the cut to 32 bits; the flash is a fixed mapping at the flash address that the stand-in
 * controller erases and programs by the rules of the chip (a flash word is programmed only where it is erased). The firmware is not touched.
 *
 *  - Flash_WriteVerified: the bytes land and the padding is the erased value, for every length around the flash word; a place that is not erased is
 *    refused and left as it was; a failing program call, a stuck bit that the read-back finds, and an unlock that is always locked again;
 *  - Flash_CopyRegion: the destination sectors are erased once each, the copy equals the source, the source is untouched, a heartbeat per page with the
 *    percentage ending in 100, and the three failures (erase, program, a range outside the bank);
 *  - the metadata log: the first record, the repeated state that writes nothing, a new state that appends with the next generation, a torn newest
 *    record that falls back to the previous one, a full sector that is erased before the first record of the new log, and a failed erase that leaves
 *    the old log readable.
 *
 * Needs Linux on x86-64 (a fixed mapping at the flash address, ucontext, MAP_32BIT); anywhere else it reports that and passes.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__) && defined(__x86_64__)
#include <sys/mman.h>
#include <ucontext.h>

#include "stm32h7xx_hal.h"

#if defined(HYDRA_TEST_CM4)
#define BANK_BASE_ADDR      0x08100000UL
#define EXPECTED_BANK       FLASH_BANK_2
#define TARGET_NAME         "CM4 (bank 2)"
#elif defined(HYDRA_TEST_CM7)
#define BANK_BASE_ADDR      0x08000000UL
#define EXPECTED_BANK       FLASH_BANK_1
#define TARGET_NAME         "CM7 (bank 1)"
#else
#error "define HYDRA_TEST_CM4 or HYDRA_TEST_CM7"
#endif

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
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

/* ---- the flash controller: both banks, 2 MB from the flash address ------------------------------------------------------------ */
#define FLASH_ALL_BASE   0x08000000UL
#define FLASH_ALL_BYTES  0x00200000UL
#define WORD_BYTES       32u

static uint8_t *flash_mem;
static int unlocks, locks, iwdg_refreshes;
static int erase_calls, program_calls;
static int fail_erase_on_call, fail_program_on_call;
static uint32_t stuck_address;       /* after a program that covers this address one bit of it reads wrong; 0 = none */
static uint32_t erased_sectors[256];
static int heartbeats;
static uint8_t heartbeat_status[512], heartbeat_percent[512];

HAL_StatusTypeDef HAL_FLASH_Unlock(void) { unlocks++; return HAL_OK; }
HAL_StatusTypeDef HAL_FLASH_Lock(void) { locks++; return HAL_OK; }
HAL_StatusTypeDef HAL_IWDG_Refresh(IWDG_HandleTypeDef *h) { (void)h; iwdg_refreshes++; return HAL_OK; }

void CAN_SendHeartbeat(uint8_t status, uint8_t progress_percent)
{
    if (heartbeats < 512) {
        heartbeat_status[heartbeats] = status;
        heartbeat_percent[heartbeats] = progress_percent;
    }
    heartbeats++;
}

HAL_StatusTypeDef HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef *init, uint32_t *sector_error)
{
    erase_calls++;
    *sector_error = 0xFFFFFFFFUL;
    if (init->TypeErase != FLASH_TYPEERASE_SECTORS || init->NbSectors != 1U || init->VoltageRange != FLASH_VOLTAGE_RANGE_3 ||
        init->Banks != EXPECTED_BANK || init->Sector > 7U) {
        *sector_error = init->Sector;
        return HAL_ERROR;
    }
    if (fail_erase_on_call && erase_calls == fail_erase_on_call) {
        *sector_error = init->Sector;
        return HAL_ERROR;
    }
    if (erase_calls <= 256) erased_sectors[erase_calls - 1] = init->Sector;
    memset(flash_mem + (BANK_BASE_ADDR - FLASH_ALL_BASE) + init->Sector * FLASH_SECTOR_SIZE, 0xFF, FLASH_SECTOR_SIZE);
    return HAL_OK;
}

HAL_StatusTypeDef HAL_FLASH_Program(uint32_t type, uint32_t address, uint32_t data_address)
{
    program_calls++;
    if (type != FLASH_TYPEPROGRAM_FLASHWORD || (address % WORD_BYTES) != 0) return HAL_ERROR;
    if (address < FLASH_ALL_BASE || address + WORD_BYTES > FLASH_ALL_BASE + FLASH_ALL_BYTES) return HAL_ERROR;
    if (fail_program_on_call && program_calls == fail_program_on_call) return HAL_ERROR;
    uint8_t *cell = flash_mem + (address - FLASH_ALL_BASE);
    for (unsigned i = 0; i < WORD_BYTES; i++) {
        if (cell[i] != 0xFF) return HAL_ERROR;      /* a flash word is programmed once between two erases */
    }
    memcpy(cell, (const void *)(uintptr_t)data_address, WORD_BYTES);
    if (stuck_address != 0 && stuck_address >= address && stuck_address < address + WORD_BYTES) {
        flash_mem[stuck_address - FLASH_ALL_BASE] ^= 0x04;
    }
    return HAL_OK;
}

#include "bootloader_flash.c"

/* ---- helpers ------------------------------------------------------------------------------------------------------------------- */
static void reset_world(void)
{
    memset(flash_mem, 0xFF, FLASH_ALL_BYTES);
    unlocks = locks = iwdg_refreshes = 0;
    erase_calls = program_calls = 0;
    fail_erase_on_call = fail_program_on_call = 0;
    stuck_address = 0;
    heartbeats = 0;
}

static uint8_t *at(uint32_t address) { return flash_mem + (address - FLASH_ALL_BASE); }

static void pattern(uint8_t *out, uint32_t len, uint32_t seed)
{
    for (uint32_t i = 0; i < len; i++) out[i] = (uint8_t)(((i + 1u) * (seed | 1u) + (i >> 8)) & 0xFFu);
}

static int is_erased(uint32_t address, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        if (*at(address + i) != 0xFF) return 0;
    }
    return 1;
}

/* ---- Flash_WriteVerified ----------------------------------------------------------------------------------------------------- */
static void test_the_bytes_land_and_the_padding_is_the_erased_value(void)
{
    printf("  Flash_WriteVerified: every length around the flash word\n");
    static const uint32_t lengths[] = {1, 4, 31, 32, 33, 63, 64, 65, 96, 100, 2048};
    for (unsigned k = 0; k < sizeof(lengths) / sizeof(lengths[0]); k++) {
        uint32_t len = lengths[k];
        uint8_t buf[2048];
        reset_world();
        pattern(buf, len, k + 3);
        CHECK(Flash_WriteVerified(MAIN_APP_ADDR, buf, len) == 1, "the write is reported good");
        CHECK(memcmp(at(MAIN_APP_ADDR), buf, len) == 0, "the bytes are in the flash");
        uint32_t padded = ((len + WORD_BYTES - 1u) / WORD_BYTES) * WORD_BYTES;
        CHECK(is_erased(MAIN_APP_ADDR + len, padded - len), "the rest of the last flash word is the erased value");
        CHECK(is_erased(MAIN_APP_ADDR + padded, 64), "and nothing after it was touched");
        CHECK(program_calls == (int)(padded / WORD_BYTES), "one program call per flash word");
        CHECK(unlocks == locks && unlocks > 0, "the controller is locked again after every unlock");
    }
}

static void test_an_empty_write_does_nothing(void)
{
    printf("  Flash_WriteVerified: a length of 0\n");
    reset_world();
    uint8_t one = 0;
    CHECK(Flash_WriteVerified(MAIN_APP_ADDR, &one, 0) == 1, "nothing to write is a success");
    CHECK(program_calls == 0 && unlocks == locks, "and no word was programmed");
}

static void test_a_place_that_is_not_erased_is_refused_and_left_as_it_was(void)
{
    printf("  Flash_WriteVerified: a place that is not erased\n");
    uint8_t first[64], second[64];
    reset_world();
    pattern(first, sizeof(first), 5);
    pattern(second, sizeof(second), 9);
    CHECK(Flash_WriteVerified(MAIN_APP_ADDR, first, sizeof(first)) == 1, "the first write");
    CHECK(Flash_WriteVerified(MAIN_APP_ADDR, second, sizeof(second)) == 0, "the second write over it is refused");
    CHECK(memcmp(at(MAIN_APP_ADDR), first, sizeof(first)) == 0, "and the first bytes are still there");
    CHECK(unlocks == locks, "the controller is locked again after a refusal");
}

static void test_a_failing_program_call_fails_the_write(void)
{
    printf("  Flash_WriteVerified: a program call that fails\n");
    uint8_t buf[200];
    for (int fail_on = 1; fail_on <= 4; fail_on++) {
        reset_world();
        pattern(buf, sizeof(buf), 7);
        fail_program_on_call = fail_on;
        CHECK(Flash_WriteVerified(MAIN_APP_ADDR, buf, sizeof(buf)) == 0, "the write is reported bad, wherever the failure is");
        CHECK(unlocks == locks, "the controller is locked again");
    }
}

static void test_a_stuck_bit_is_found_by_the_read_back(void)
{
    printf("  Flash_WriteVerified: a bit that does not hold\n");
    uint8_t buf[200];
    for (uint32_t offset = 0; offset < sizeof(buf); offset += 37) {
        reset_world();
        pattern(buf, sizeof(buf), 11);
        stuck_address = MAIN_APP_ADDR + offset;
        CHECK(Flash_WriteVerified(MAIN_APP_ADDR, buf, sizeof(buf)) == 0, "the read-back finds the byte that reads wrong");
    }
}

/* ---- Flash_CopyRegion --------------------------------------------------------------------------------------------------------- */
static void test_a_copy_equals_the_source_and_leaves_it_alone(void)
{
    printf("  Flash_CopyRegion: sizes from one byte to the whole slot\n");
    static uint8_t source[APP_MAX_SIZE];
    static const uint32_t sizes[] = {1, 2047, 2048, 2049, 5000, 131072, 131073, 300000, APP_MAX_SIZE};
    for (unsigned k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
        uint32_t size = sizes[k];
        reset_world();
        pattern(source, size, k + 2);
        memcpy(at(BACKUP_APP_ADDR), source, size);          /* the backup slot is erased flash that was programmed once */
        memset(at(MAIN_APP_ADDR), 0x00, 4096);              /* old content in the destination: the erase has to clear it */
        CHECK(Flash_CopyRegion(MAIN_APP_ADDR, BACKUP_APP_ADDR, size) == 1, "the copy is reported good");
        CHECK(memcmp(at(MAIN_APP_ADDR), source, size) == 0, "the destination equals the source");
        CHECK(memcmp(at(BACKUP_APP_ADDR), source, size) == 0, "the source is untouched");
        uint32_t first = (MAIN_APP_ADDR - BANK_BASE_ADDR) / FLASH_SECTOR_SIZE;
        uint32_t last = (MAIN_APP_ADDR + size - 1u - BANK_BASE_ADDR) / FLASH_SECTOR_SIZE;
        CHECK(erase_calls == (int)(last - first + 1u), "every destination sector is erased, once");
        for (int e = 0; e < erase_calls && e < 256; e++) {
            CHECK(erased_sectors[e] == first + (uint32_t)e, "in order, from the first sector of the destination");
        }
        uint32_t pages = (size + FLASH_PAGE_SIZE - 1u) / FLASH_PAGE_SIZE;
        CHECK(heartbeats == (int)pages, "one heartbeat per page");
        if (heartbeats > 0 && heartbeats <= 512) {
            CHECK(heartbeat_percent[heartbeats - 1] == 100, "the last heartbeat says 100");
            CHECK(heartbeat_status[0] == STATUS_COPYING, "they say the copy is running");
            int rising = 1;
            for (int i = 1; i < heartbeats; i++) {
                if (heartbeat_percent[i] < heartbeat_percent[i - 1]) rising = 0;
            }
            CHECK(rising, "and the percentage never goes back");
        }
        CHECK(unlocks == locks, "the controller is locked again");
    }
}

static void test_a_copy_that_fails_says_so(void)
{
    printf("  Flash_CopyRegion: the erase fails, the program fails\n");
    static uint8_t source[8192];
    reset_world();
    pattern(source, sizeof(source), 3);
    memcpy(at(BACKUP_APP_ADDR), source, sizeof(source));
    fail_erase_on_call = 1;
    CHECK(Flash_CopyRegion(MAIN_APP_ADDR, BACKUP_APP_ADDR, sizeof(source)) == 0, "a failed erase is a failed copy");
    CHECK(program_calls == 0 && heartbeats == 0, "and nothing was written");
    CHECK(unlocks == locks, "the controller is locked again");

    reset_world();
    memcpy(at(BACKUP_APP_ADDR), source, sizeof(source));
    fail_program_on_call = 70;                              /* 64 words per page: the second page fails */
    CHECK(Flash_CopyRegion(MAIN_APP_ADDR, BACKUP_APP_ADDR, sizeof(source)) == 0, "a failed program call is a failed copy");
    CHECK(heartbeats == 1, "the first page was reported, the second never");
    CHECK(memcmp(at(BACKUP_APP_ADDR), source, sizeof(source)) == 0, "the source is untouched");

    reset_world();
    memcpy(at(BACKUP_APP_ADDR), source, sizeof(source));
    stuck_address = MAIN_APP_ADDR + 3000;
    CHECK(Flash_CopyRegion(MAIN_APP_ADDR, BACKUP_APP_ADDR, sizeof(source)) == 0, "a bit that does not hold is a failed copy");
}

/* ---- the metadata log ----------------------------------------------------------------------------------------------------------- */
static FirmwareMetadata_t make_meta(uint32_t seed)
{
    FirmwareMetadata_t m;
    memset(&m, 0, sizeof(m));
    m.magic = METADATA_MAGIC_VALID;
    m.state = META_STATE_APP_VALID;
    m.version_major = seed;
    m.version_minor = seed + 1u;
    m.size = 1000u + seed;
    m.crc32 = 0xA5A50000UL + seed;
    memset(m.sha256, (int)(seed & 0xFFu), sizeof(m.sha256));
    return m;
}

static void poke_record(uint32_t index, uint32_t generation, const FirmwareMetadata_t *meta)
{
    MetaRecord_t record;
    memset(&record, 0, sizeof(record));
    record.generation = generation;
    record.meta = *meta;
    record.crc = meta_record_crc(&record);
    memcpy(at(METADATA_ADDR + index * META_RECORD_SIZE), &record, sizeof(record));
}

static int same_meta(const FirmwareMetadata_t *a, const FirmwareMetadata_t *b) { return memcmp(a, b, sizeof(*a)) == 0; }

static void test_the_log_starts_empty_and_takes_a_first_record(void)
{
    printf("  the metadata log: nothing, then a first record\n");
    reset_world();
    FirmwareMetadata_t got;
    memset(&got, 0x5A, sizeof(got));
    CHECK(Metadata_Read(&got) == 0, "an erased sector has no record");
    FirmwareMetadata_t zero;
    memset(&zero, 0, sizeof(zero));
    CHECK(same_meta(&got, &zero), "and the answer is cleared");

    FirmwareMetadata_t first = make_meta(1);
    CHECK(Metadata_EraseAndWrite(&first) == 1, "the first record is written");
    CHECK(Metadata_Read(&got) == 1 && same_meta(&got, &first), "and read back as written");
    CHECK(meta_slot(0)->generation == 1, "it is generation 1 in the first slot");
    CHECK(erase_calls == 0, "the sector was not erased for it");
}

static void test_a_state_that_is_already_the_last_writes_nothing(void)
{
    printf("  the metadata log: the same state again\n");
    reset_world();
    FirmwareMetadata_t m = make_meta(2);
    CHECK(Metadata_EraseAndWrite(&m) == 1, "the first write");
    int programs = program_calls;
    CHECK(Metadata_EraseAndWrite(&m) == 1, "the same state again is a success");
    CHECK(program_calls == programs, "and writes nothing");
    CHECK(is_erased(METADATA_ADDR + META_RECORD_SIZE, META_RECORD_SIZE), "the next slot is still blank");
}

static void test_a_new_state_appends_with_the_next_generation(void)
{
    printf("  the metadata log: a new state appends\n");
    reset_world();
    FirmwareMetadata_t a = make_meta(3), b = make_meta(4), c = make_meta(5), got;
    CHECK(Metadata_EraseAndWrite(&a) == 1 && Metadata_EraseAndWrite(&b) == 1 && Metadata_EraseAndWrite(&c) == 1, "three states");
    CHECK(meta_slot(0)->generation == 1 && meta_slot(1)->generation == 2 && meta_slot(2)->generation == 3, "generations 1, 2 and 3 in the first three slots");
    CHECK(Metadata_Read(&got) == 1 && same_meta(&got, &c), "the newest is the one that is read");
    CHECK(erase_calls == 0, "none of this erased the sector");
    CHECK(unlocks == locks, "the controller is locked again");
}

static void test_a_torn_newest_record_falls_back_to_the_previous_one(void)
{
    printf("  the metadata log: a record cut by a power loss\n");
    reset_world();
    FirmwareMetadata_t a = make_meta(6), b = make_meta(7), got;
    CHECK(Metadata_EraseAndWrite(&a) == 1 && Metadata_EraseAndWrite(&b) == 1, "two states");
    *at(METADATA_ADDR + META_RECORD_SIZE + 10) ^= 0x01;      /* the newest record no longer matches its CRC */
    CHECK(Metadata_Read(&got) == 1 && same_meta(&got, &a), "the previous record is read");

    reset_world();
    CHECK(Metadata_EraseAndWrite(&a) == 1, "a state");
    *at(METADATA_ADDR + 12) ^= 0x80;
    CHECK(Metadata_Read(&got) == 0, "when the only record is torn there is none");
}

#if !HYDRA_META_AB
static void test_a_full_sector_is_erased_before_the_first_record_of_the_next_log(void)
{
    printf("  the metadata log: the sector is full\n");
    reset_world();
    FirmwareMetadata_t got;
    for (uint32_t i = 0; i < META_RECORD_SLOTS; i++) {
        FirmwareMetadata_t m = make_meta(100u + i);
        poke_record(i, 1u + i, &m);
    }
    FirmwareMetadata_t last = make_meta(100u + META_RECORD_SLOTS - 1u);
    CHECK(Metadata_Read(&got) == 1 && same_meta(&got, &last), "the last record of a full log is the newest");

    FirmwareMetadata_t next = make_meta(5000);
    CHECK(Metadata_EraseAndWrite(&next) == 1, "the write after a full log works");
    CHECK(erase_calls == 1 && erased_sectors[0] == (METADATA_ADDR - BANK_BASE_ADDR) / FLASH_SECTOR_SIZE, "the metadata sector, and only it, was erased");
    CHECK(meta_slot(0)->generation == META_RECORD_SLOTS + 1u, "the new log starts with the next generation, in the first slot");
    CHECK(Metadata_Read(&got) == 1 && same_meta(&got, &next), "and it is the one that is read");
    CHECK(is_erased(METADATA_ADDR + META_RECORD_SIZE, META_RECORD_SIZE), "the second slot is blank");
}
#endif

static void test_a_failed_erase_leaves_the_old_log_readable(void)
{
    printf("  the metadata log: the erase of a full sector fails\n");
    reset_world();
    FirmwareMetadata_t got;
    for (uint32_t i = 0; i < META_RECORD_SLOTS; i++) {
        FirmwareMetadata_t m = make_meta(200u + i);
        poke_record(i, 1u + i, &m);
    }
    fail_erase_on_call = 1;
    FirmwareMetadata_t next = make_meta(6000);
    CHECK(Metadata_EraseAndWrite(&next) == 0, "the write is reported bad");
    FirmwareMetadata_t last = make_meta(200u + META_RECORD_SLOTS - 1u);
    CHECK(Metadata_Read(&got) == 1 && same_meta(&got, &last), "the log that was full is still read");
}

#if !HYDRA_META_AB
static void test_a_cut_after_the_erase_of_a_full_log_loses_the_rollback_counter(void)
{
    /* The twin of the weakness of the single metadata sector: when the log is full the sector is erased and the new record is written after it, so a power
     * cut between the two leaves no record at all and the board reads as a first boot, with the rollback counter back at the floor compiled into the
     * bootloader. This test states what happens TODAY; the layout of two alternating sectors (HYDRA_META_AB) is the change that makes it keep the counter,
     * and this is the case it has to turn around. */
    printf("  the metadata log: a power cut right after the erase of a full sector\n");
    reset_world();
    FirmwareMetadata_t got;
    for (uint32_t i = 0; i < META_RECORD_SLOTS; i++) {
        FirmwareMetadata_t m = make_meta(300u + i);
        m.counter = 7u;
        poke_record(i, 1u + i, &m);
    }
    CHECK(Metadata_Read(&got) == 1 && got.counter == 7u, "before the cut the log holds the counter");
    FirmwareMetadata_t next = make_meta(7000);
    next.counter = 8u;
    fail_program_on_call = program_calls + 1;    /* the erase works, the first program call after it is the cut */
    CHECK(Metadata_EraseAndWrite(&next) == 0, "the write is reported bad");
    CHECK(erase_calls == 1, "the sector had been erased by then");
    CHECK(Metadata_Read(&got) == 0, "no record is left: the board reads as a first boot");
    CHECK(got.counter == 0u, "and the rollback counter is gone with it");
}
#endif

static void test_a_write_that_fails_adds_no_record(void)
{
    printf("  the metadata log: the program call fails\n");
    reset_world();
    FirmwareMetadata_t a = make_meta(8), b = make_meta(9), got;
    CHECK(Metadata_EraseAndWrite(&a) == 1, "a state");
    fail_program_on_call = program_calls + 1;
    CHECK(Metadata_EraseAndWrite(&b) == 0, "the next one fails");
    CHECK(Metadata_Read(&got) == 1 && same_meta(&got, &a), "and the log still ends with the first");
}

#if HYDRA_META_AB
/* ---- the metadata log in two sectors that take turns (HYDRA_META_AB=1) ----------------------------------------------------------- */
#define REGION_A METADATA_ADDR
#define REGION_B METADATA_B_ADDR

static uint32_t sector_of(uint32_t address) { return (address - BANK_BASE_ADDR) / FLASH_SECTOR_SIZE; }

static void poke_in(uint32_t base, uint32_t index, uint32_t generation, const FirmwareMetadata_t *meta)
{
    MetaRecord_t record;
    memset(&record, 0, sizeof(record));
    record.generation = generation;
    record.meta = *meta;
    record.crc = meta_record_crc(&record);
    memcpy(at(base + index * META_RECORD_SIZE), &record, sizeof(record));
}

static void fill_region(uint32_t base, uint32_t first_generation, uint32_t seed, uint32_t counter)
{
    for (uint32_t i = 0; i < META_RECORD_SLOTS; i++) {
        FirmwareMetadata_t m = make_meta(seed + i);
        m.counter = counter;
        poke_in(base, i, first_generation + i, &m);
    }
}

static void test_ab_the_second_sector_is_the_last_of_the_backup_slot(void)
{
    printf("  the two metadata sectors: where they are\n");
    CHECK(sector_of(REGION_B) == 7u && sector_of(REGION_A) == 1u, "sectors 1 and 7 of the bank");
    CHECK(REGION_B >= BACKUP_APP_ADDR + APP_MAX_SIZE, "and the longest application ends before the second one");
}

static void test_ab_a_board_with_the_single_sector_log_goes_on_in_it(void)
{
    printf("  the two metadata sectors: a board that has only the first sector (no migration)\n");
    reset_world();
    memset(at(REGION_B), 0xA5, FLASH_SECTOR_SIZE);                /* the sector was part of the backup slot: it holds an old application */
    FirmwareMetadata_t got;
    for (uint32_t i = 0; i < 40u; i++) {
        FirmwareMetadata_t m = make_meta(10u + i);
        m.counter = 3u;
        poke_in(REGION_A, i, 1u + i, &m);
    }
    CHECK(Metadata_Read(&got) == 1 && got.counter == 3u && got.version_major == 10u + 39u, "its newest record is the state");
    FirmwareMetadata_t next = make_meta(500);
    next.counter = 4u;
    CHECK(Metadata_EraseAndWrite(&next) == 1, "the next record is written");
    CHECK(erase_calls == 0, "nothing was erased");
    CHECK(meta_slot(40)->generation == 41u, "it is the next slot of the same log, with the next generation");
    CHECK(Metadata_Read(&got) == 1 && same_meta(&got, &next), "and it is read back");
    CHECK(*at(REGION_B) == 0xA5 && *at(REGION_B + FLASH_SECTOR_SIZE - 1u) == 0xA5, "the second sector is not touched");
}

static void test_ab_a_full_first_sector_goes_on_in_the_second_without_touching_itself(void)
{
    printf("  the two metadata sectors: the first one is full\n");
    reset_world();
    memset(at(REGION_B), 0xA5, FLASH_SECTOR_SIZE);
    fill_region(REGION_A, 1u, 100u, 7u);
    FirmwareMetadata_t next = make_meta(5000), got;
    next.counter = 8u;
    CHECK(Metadata_EraseAndWrite(&next) == 1, "the write works");
    CHECK(erase_calls == 1 && erased_sectors[0] == sector_of(REGION_B), "only the second sector was erased");
    CHECK(meta_slot_in(REGION_B, 0)->generation == META_RECORD_SLOTS + 1u, "the new record is the first of the second sector, with the next generation");
    CHECK(Metadata_Read(&got) == 1 && same_meta(&got, &next) && got.counter == 8u, "and it is the state");
    CHECK(meta_slot(META_RECORD_SLOTS - 1u)->generation == META_RECORD_SLOTS && meta_slot_valid(meta_slot(0)), "the full sector keeps its records");
}

static void test_ab_the_sectors_take_turns(void)
{
    printf("  the two metadata sectors: the second one fills and the first one is used again\n");
    reset_world();
    fill_region(REGION_A, 1u, 100u, 5u);
    fill_region(REGION_B, META_RECORD_SLOTS + 1u, 2000u, 6u);
    FirmwareMetadata_t next = make_meta(9000), got;
    next.counter = 7u;
    CHECK(Metadata_EraseAndWrite(&next) == 1, "the write after both are full works");
    CHECK(erase_calls == 1 && erased_sectors[0] == sector_of(REGION_A), "the first sector, which holds the older records, was erased");
    CHECK(meta_slot(0)->generation == 2u * META_RECORD_SLOTS + 1u, "the new record starts it with the next generation");
    CHECK(Metadata_Read(&got) == 1 && same_meta(&got, &next), "and it is the state");
    CHECK(meta_slot_valid(meta_slot_in(REGION_B, META_RECORD_SLOTS - 1u)), "the second sector keeps its records");
}

static void test_ab_a_cut_at_any_point_of_the_change_of_sector_keeps_the_rollback_counter(void)
{
    printf("  the two metadata sectors: a power cut at every point of the change of sector\n");
    /* A record is 96 bytes: 3 flash words. The cut is the erase, or the first, second or third program call; each is tried on its own. */
    for (int cut = 0; cut < 4; cut++) {
        reset_world();
        FirmwareMetadata_t got;
        fill_region(REGION_A, 1u, 300u, 7u);
        FirmwareMetadata_t next = make_meta(7000);
        next.counter = 8u;
        if (cut == 0) fail_erase_on_call = 1;
        else fail_program_on_call = program_calls + cut;
        CHECK(Metadata_EraseAndWrite(&next) == 0, "the write is reported bad");
        CHECK(Metadata_Read(&got) == 1, "a record is left");
        CHECK(got.counter == 7u && got.version_major == 300u + META_RECORD_SLOTS - 1u, "it is the state before the cut, with its rollback counter");
        /* power is back: the retry completes the change */
        int erases = erase_calls;
        fail_erase_on_call = fail_program_on_call = 0;
        CHECK(Metadata_EraseAndWrite(&next) == 1, "the retry works");
        if (cut == 0) CHECK(erase_calls == erases + 1, "the sector whose erase failed is erased by the retry");
        else CHECK(erase_calls == erases, "a sector that is blank already is not erased again");
        CHECK(Metadata_Read(&got) == 1 && same_meta(&got, &next) && got.counter == 8u, "the state is now the new one");
    }
}

static void test_ab_a_cut_in_the_second_sector_keeps_the_state_of_the_first(void)
{
    printf("  the two metadata sectors: a power cut on the way back to the first sector\n");
    reset_world();
    FirmwareMetadata_t got;
    fill_region(REGION_A, 1u, 100u, 5u);
    fill_region(REGION_B, META_RECORD_SLOTS + 1u, 2000u, 6u);
    FirmwareMetadata_t next = make_meta(9100);
    next.counter = 7u;
    fail_program_on_call = program_calls + 1;
    CHECK(Metadata_EraseAndWrite(&next) == 0, "the write is reported bad");
    CHECK(Metadata_Read(&got) == 1 && got.counter == 6u && got.version_major == 2000u + META_RECORD_SLOTS - 1u, "the newest record of the second sector is still the state");
}

static void test_ab_the_newest_generation_wins_whichever_sector_holds_it(void)
{
    printf("  the two metadata sectors: the newest of the two is the state\n");
    reset_world();
    FirmwareMetadata_t a = make_meta(1), b = make_meta(2), got;
    a.counter = 1u;
    b.counter = 2u;
    poke_in(REGION_A, 0, 5u, &a);
    poke_in(REGION_B, 0, 9u, &b);
    CHECK(Metadata_Read(&got) == 1 && got.counter == 2u, "the higher generation is in the second sector");
    reset_world();
    poke_in(REGION_A, 0, 9u, &a);
    poke_in(REGION_B, 0, 5u, &b);
    CHECK(Metadata_Read(&got) == 1 && got.counter == 1u, "the higher generation is in the first sector");
    CHECK(Metadata_EraseAndWrite(&a) == 1 && program_calls == 0, "the state that is the last already writes nothing");
}

static void test_ab_a_failed_erase_of_the_other_sector_leaves_the_state_readable(void)
{
    printf("  the two metadata sectors: the erase of the other sector fails\n");
    reset_world();
    FirmwareMetadata_t got;
    fill_region(REGION_A, 1u, 200u, 4u);
    memset(at(REGION_B), 0xA5, FLASH_SECTOR_SIZE);
    fail_erase_on_call = 1;
    FirmwareMetadata_t next = make_meta(6000);
    CHECK(Metadata_EraseAndWrite(&next) == 0, "the write is reported bad");
    CHECK(Metadata_Read(&got) == 1 && got.counter == 4u && got.version_major == 200u + META_RECORD_SLOTS - 1u, "the full sector is still the state");
}
#endif /* HYDRA_META_AB */

/* ---- the driver: all of the above on a stack in the low 2 GB -------------------------------------------------------------------- */
static void run_all(void)
{
    printf("  target: %s%s\n", TARGET_NAME, HYDRA_FLASH_LAYER ? ", with the common flash layer" : "");
    test_the_bytes_land_and_the_padding_is_the_erased_value();
    test_an_empty_write_does_nothing();
    test_a_place_that_is_not_erased_is_refused_and_left_as_it_was();
    test_a_failing_program_call_fails_the_write();
    test_a_stuck_bit_is_found_by_the_read_back();
    test_a_copy_equals_the_source_and_leaves_it_alone();
    test_a_copy_that_fails_says_so();
    test_the_log_starts_empty_and_takes_a_first_record();
    test_a_state_that_is_already_the_last_writes_nothing();
    test_a_new_state_appends_with_the_next_generation();
    test_a_torn_newest_record_falls_back_to_the_previous_one();
#if !HYDRA_META_AB
    test_a_full_sector_is_erased_before_the_first_record_of_the_next_log();
#endif
    test_a_failed_erase_leaves_the_old_log_readable();
#if !HYDRA_META_AB
    test_a_cut_after_the_erase_of_a_full_log_loses_the_rollback_counter();
#endif
    test_a_write_that_fails_adds_no_record();
#if HYDRA_META_AB
    test_ab_the_second_sector_is_the_last_of_the_backup_slot();
    test_ab_a_board_with_the_single_sector_log_goes_on_in_it();
    test_ab_a_full_first_sector_goes_on_in_the_second_without_touching_itself();
    test_ab_the_sectors_take_turns();
    test_ab_a_cut_at_any_point_of_the_change_of_sector_keeps_the_rollback_counter();
    test_ab_a_cut_in_the_second_sector_keeps_the_state_of_the_first();
    test_ab_the_newest_generation_wins_whichever_sector_holds_it();
    test_ab_a_failed_erase_of_the_other_sector_leaves_the_state_readable();
#endif
}

static ucontext_t main_context, test_context;

int main(void)
{
    printf("HYDRA-UMC host tests - H745 bootloader flash write, copy and metadata log\n");

    flash_mem = mmap((void *)FLASH_ALL_BASE, FLASH_ALL_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (flash_mem == MAP_FAILED || (uintptr_t)flash_mem != FLASH_ALL_BASE) {
        printf("  SKIP: cannot map the flash address range on this host\n");
        return 0;
    }
    const size_t stack_bytes = 4u * 1024u * 1024u;
    void *stack = mmap(NULL, stack_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (stack == MAP_FAILED || (uintptr_t)stack + stack_bytes > 0x7FFFFFFFUL) {
        printf("  SKIP: cannot get a stack in the low 2 GB on this host\n");
        return 0;
    }
    if (getcontext(&test_context) != 0) {
        printf("  SKIP: no ucontext on this host\n");
        return 0;
    }
    test_context.uc_stack.ss_sp = stack;
    test_context.uc_stack.ss_size = stack_bytes;
    test_context.uc_link = &main_context;
    makecontext(&test_context, run_all, 0);
    if (swapcontext(&main_context, &test_context) != 0) {
        printf("  FAIL: cannot switch to the low stack\n");
        return 1;
    }

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all H745 bootloader flash host tests passed\n");
    return 0;
}

#else /* not Linux on x86-64 */

int main(void)
{
    printf("HYDRA-UMC host tests - H745 bootloader flash: skipped (needs Linux on x86-64)\n");
    return 0;
}

#endif
