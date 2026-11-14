/* =============================================================================
 * HYDRA-UMC Firmware - Host tests of the G474 bootloader outside the update: the read-back of the application, the validity check at boot,
 * the answers to the queries and the refusals to jump
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * bootloader_protocol.c is compiled as it is against tests/stubs/protocol/stm32g4xx_hal.h (the same stand-in the whole-update test uses); this file
 * plays the CAN bus (it records what the bootloader sends and answers the per-page acknowledgement of the read-back), the flash controller and the
 * clock. Nothing runs on a board. What it covers, none of which the update test reaches:
 *
 *  - the read-back (HandleReadbackStart): it serves the installed application page by page, waits for the acknowledgement of each page and gives up
 *    after three seconds when none comes (no frame, a frame of another identifier, of another length, of another page, a read error); it serves
 *    nothing when there is no valid installed record (no metadata, a copy still pending, another board, a size of 0 or over the slot), nothing during an
 *    update, and with the read-out protection gate on (HYDRA_READBACK_GATE=1) nothing when the protection is on, only the length 0;
 *  - ApplicationIsValid: the adoption of an application that has no record yet (a plausible stack pointer, the refusal of an implausible one and of a
 *    flash write that does not take), a record that matches the flash, a flipped byte (CRC), a record whose digest does not match, a record of another board
 *    or with an impossible size, and the copy that a pending record asks for;
 *  - the version and the error-counter queries, the status, the heartbeat and the scan reply, and what happens when no transmit mailbox frees up;
 *  - JumpToApplication: the five vectors it refuses (it never reaches the code that leaves the bootloader).
 *
 * Needs Linux on x86-64 (a fixed mapping at the flash address); anywhere else it reports that and passes.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__) && defined(__x86_64__)
#include <sys/mman.h>

#define HYDRA_TRUST_DEV 1
#define HYDRA_HOST_TEST 1

#include "stm32g4xx_hal.h"
#include "tweetnacl.h"

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

/* ---- what the bootloader sources expect the board to define ------------------------------------------------------------------ */
FDCAN_HandleTypeDef hfdcan1;
IWDG_HandleTypeDef hiwdg;
FLASH_TypeDef host_flash_registers;
GPIO_TypeDef host_gpioa, host_gpiob;
SysTick_Type host_systick;
SCB_Type host_scb;
static FDCAN_GlobalTypeDef fdcan_registers;

static uint32_t clock_ms = 1000;
static int resets;
static int deinit_calls;

uint32_t HAL_GetTick(void) { return clock_ms; }
void HAL_Delay(uint32_t ms) { clock_ms += ms; }
HAL_StatusTypeDef HAL_IWDG_Refresh(IWDG_HandleTypeDef *h) { (void)h; return HAL_OK; }
void HAL_NVIC_DisableIRQ(int irq) { (void)irq; deinit_calls++; }
HAL_StatusTypeDef HAL_FDCAN_DeInit(FDCAN_HandleTypeDef *h) { (void)h; deinit_calls++; return HAL_OK; }
void HAL_RCC_DeInit(void) { deinit_calls++; }
void HAL_DeInit(void) { deinit_calls++; }
void __disable_irq(void) {}
void __set_MSP(uint32_t top) { (void)top; }
void NVIC_SystemReset(void) { resets++; }

/* ---- the CAN bus ------------------------------------------------------------------------------------------------------------- */
typedef struct { uint32_t id; uint32_t dlc; uint8_t data[8]; } Frame;
static Frame sent[4096];
static int sent_count;
static int tx_busy_polls;         /* how many polls of the transmit FIFO answer "full" before it frees up */

/* What the host answers on the receive side while the bootloader waits for the acknowledgement of a page. */
enum { RX_NONE, RX_ACK, RX_WRONG_ID, RX_WRONG_PAGE, RX_WRONG_LENGTH, RX_READ_ERROR };
static int rx_mode;
static uint32_t rx_page;
static uint32_t slot_base_for_acks;

uint32_t HAL_FDCAN_GetTxFifoFreeLevel(FDCAN_HandleTypeDef *h)
{
    (void)h;
    if (tx_busy_polls > 0) {
        tx_busy_polls--;
        return 0;
    }
    return 1;
}

HAL_StatusTypeDef HAL_FDCAN_AddMessageToTxFifoQ(FDCAN_HandleTypeDef *h, FDCAN_TxHeaderTypeDef *header, uint8_t *data)
{
    (void)h;
    if (sent_count < 4096) {
        sent[sent_count].id = header->Identifier;
        sent[sent_count].dlc = header->DataLength;
        memcpy(sent[sent_count].data, data, 8);
        sent_count++;
    }
    return HAL_OK;
}

uint32_t HAL_FDCAN_GetRxFifoFillLevel(FDCAN_HandleTypeDef *h, uint32_t fifo)
{
    (void)h; (void)fifo;
    if (rx_mode == RX_NONE) {
        clock_ms += 1;   /* nothing arrives: the time passes, as it would on the board */
        return 0;
    }
    return 1;
}

HAL_StatusTypeDef HAL_FDCAN_GetRxMessage(FDCAN_HandleTypeDef *h, uint32_t fifo, FDCAN_RxHeaderTypeDef *header, uint8_t *data)
{
    (void)h; (void)fifo;
    clock_ms += 1;
    if (rx_mode == RX_READ_ERROR) return HAL_ERROR;
    uint32_t page = rx_page;
    header->IdType = FDCAN_STANDARD_ID;
    header->RxFrameType = FDCAN_DATA_FRAME;
    header->Identifier = slot_base_for_acks + OFS_BACKUP_READ_PAGE_ACK;
    header->DataLength = 4;
    if (rx_mode == RX_WRONG_ID) header->Identifier += 1;
    if (rx_mode == RX_WRONG_LENGTH) header->DataLength = 3;
    if (rx_mode == RX_WRONG_PAGE) page += 1;
    data[0] = (uint8_t)(page >> 24); data[1] = (uint8_t)(page >> 16); data[2] = (uint8_t)(page >> 8); data[3] = (uint8_t)page;
    if (rx_mode == RX_ACK) rx_page++;
    return HAL_OK;
}

/* ---- the flash controller -------------------------------------------------------------------------------------------------------- */
#define FLASH_BASE_ADDR  0x08000000UL
#define FLASH_TOTAL      0x00080000UL
#define BANK_BYTES       0x00040000UL

static uint8_t *flash_mem;
static int is_unlocked;
static int fail_program;
static int fail_erase;

HAL_StatusTypeDef HAL_FLASH_Unlock(void) { is_unlocked = 1; return HAL_OK; }
HAL_StatusTypeDef HAL_FLASH_Lock(void) { is_unlocked = 0; return HAL_OK; }

HAL_StatusTypeDef HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef *init, uint32_t *page_error)
{
    *page_error = 0xFFFFFFFFUL;
    if (fail_erase) return HAL_ERROR;
    if (!is_unlocked || init->TypeErase != FLASH_TYPEERASE_PAGES || init->NbPages != 1U) return HAL_ERROR;
    if ((init->Banks != FLASH_BANK_1 && init->Banks != FLASH_BANK_2) || init->Page > 127U) return HAL_ERROR;
    uint32_t offset = (init->Banks == FLASH_BANK_2 ? BANK_BYTES : 0UL) + init->Page * FLASH_PAGE_SIZE;
    memset(flash_mem + offset, 0xFF, FLASH_PAGE_SIZE);
    return HAL_OK;
}

HAL_StatusTypeDef HAL_FLASH_Program(uint32_t type, uint32_t address, uint64_t data)
{
    if (fail_program) return HAL_ERROR;
    if (!is_unlocked || type != FLASH_TYPEPROGRAM_DOUBLEWORD) return HAL_ERROR;
    if ((address & 7UL) != 0 || address < FLASH_BASE_ADDR || address + 8 > FLASH_BASE_ADDR + FLASH_TOTAL) return HAL_ERROR;
    uint8_t *cell = flash_mem + (address - FLASH_BASE_ADDR);
    for (int i = 0; i < 8; i++) {
        if (cell[i] != 0xFF) return HAL_ERROR;
    }
    memcpy(cell, &data, 8);
    return HAL_OK;
}

/* The sources under test. */
#include "bootloader_crypto.c"
#include "bootloader_flash.c"
#include "bootloader_trust.c"
#include "bootloader_protocol.c"

/* ---- helpers ------------------------------------------------------------------------------------------------------------------- */
#define SLOT_BASE 0x600UL
#define RDP_LEVEL0 0xAAu
#define RDP_LEVEL1 0xBBu
#define RDP_LEVEL2 0xCCu

static uint8_t *main_slot(void) { return flash_mem + (MAIN_APP_ADDR - FLASH_BASE_ADDR); }
static uint8_t *backup_slot(void) { return flash_mem + (BACKUP_APP_ADDR - FLASH_BASE_ADDR); }

static void put_le32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static void reset_world(void)
{
    memset(flash_mem, 0xFF, FLASH_TOTAL);
    host_flash_registers.OPTR = FLASH_OPTR_DBANK | RDP_LEVEL0;
    fdcan_registers.ECR = 0;
    is_unlocked = 0;
    fail_program = 0;
    fail_erase = 0;
    sent_count = 0;
    resets = 0;
    deinit_calls = 0;
    clock_ms = 1000;
    tx_busy_polls = 0;
    rx_mode = RX_NONE;
    rx_page = 0;
    slot_base_for_acks = SLOT_BASE;
    update_in_progress = 0;
    update_failed = 0;
    hfdcan1.Instance = &fdcan_registers;
    Protocol_Init(SLOT_BASE);
}

/* An application of `size` bytes at `where` (the main or the backup slot): the vectors of a plausible image, then a pattern. */
static void lay_out_app(uint8_t *where, uint32_t size, uint32_t seed)
{
    for (uint32_t i = 0; i < size; i++) where[i] = (uint8_t)((i * seed + 7u) & 0xFFu);
    put_le32(where, 0x20010000UL);
    put_le32(where + 4, MAIN_APP_ADDR + 0x101UL);
}

static void write_record(const FirmwareMetadata_t *m)
{
    MetaRecord_t record;
    memset(&record, 0, sizeof(record));
    record.generation = 1;
    record.meta = *m;
    record.crc = meta_record_crc(&record);
    memcpy(flash_mem + (METADATA_ADDR - FLASH_BASE_ADDR), &record, sizeof(record));
}

/* The record of an application that sits in the slot at `address`: its CRC32 and its SHA-256 as the bootloader computes them. */
static FirmwareMetadata_t record_for(uint32_t address, uint32_t state, uint32_t hw_id, uint32_t size)
{
    FirmwareMetadata_t m;
    memset(&m, 0, sizeof(m));
    m.magic = METADATA_MAGIC_VALID;
    m.state = state;
    m.hardware_id = hw_id;
    m.version_major = 1;
    m.version_minor = 2;
    m.version_patch = 3;
    m.size = size;
    uint32_t crc = CRC32_Update(0xFFFFFFFFUL, (const uint8_t *)address, size);
    m.crc32 = CRC32_Finalize(crc);
    sha256_prefixed_flash_region(NULL, 0, address, size, m.sha256);
    return m;
}

/* An installed application: the bytes in the main slot and the record that describes them. */
static void install_app(uint32_t size, uint32_t seed)
{
    lay_out_app(main_slot(), size, seed);
    FirmwareMetadata_t m = record_for(MAIN_APP_ADDR, META_STATE_APP_VALID, THIS_HARDWARE_ID, size);
    write_record(&m);
}

static FirmwareMetadata_t metadata(void)
{
    FirmwareMetadata_t m;
    Metadata_Read(&m);
    return m;
}

static int count_frames(uint32_t offset)
{
    int n = 0;
    for (int i = 0; i < sent_count; i++) {
        if (sent[i].id == SLOT_BASE + offset) n++;
    }
    return n;
}

/* The bytes that the read-back put on the bus after the length frame, in order; returns how many. */
static uint32_t collected_bytes(uint8_t *out, uint32_t max)
{
    uint32_t n = 0;
    int seen_length = 0;
    for (int i = 0; i < sent_count; i++) {
        if (sent[i].id != SLOT_BASE + OFS_BACKUP_READ_RESPONSE) continue;
        if (!seen_length) {
            seen_length = 1;
            continue;
        }
        for (uint32_t k = 0; k < sent[i].dlc && n < max; k++) out[n++] = sent[i].data[k];
    }
    return n;
}

static uint32_t announced_length(void)
{
    for (int i = 0; i < sent_count; i++) {
        if (sent[i].id != SLOT_BASE + OFS_BACKUP_READ_RESPONSE) continue;
        if (sent[i].dlc != 4) return 0xFFFFFFFFUL;
        return ((uint32_t)sent[i].data[0] << 24) | ((uint32_t)sent[i].data[1] << 16) | ((uint32_t)sent[i].data[2] << 8) | sent[i].data[3];
    }
    return 0xFFFFFFFEUL;   /* no frame at all */
}

/* ---- the read-back ----------------------------------------------------------------------------------------------------------- */
static void test_the_readback_serves_every_page_of_the_installed_application(void)
{
    printf("  the read-back serves the installed application, page by page, as the host acknowledges each one\n");
    static uint8_t got[8192];
    const uint32_t sizes[] = {8, 100, 2048, 2049, 5000};
    for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        reset_world();
        install_app(sizes[s], 5);
        rx_mode = RX_ACK;
        HandleReadbackStart();
        CHECK(announced_length() == sizes[s], "the first frame announces the size of the installed application");
        uint32_t n = collected_bytes(got, sizeof(got));
        CHECK(n == sizes[s], "exactly the installed bytes are served, no more");
        CHECK(memcmp(got, main_slot(), sizes[s]) == 0, "and they are the bytes of the main slot");
        uint32_t frames = (sizes[s] + 7u) / 8u;
        uint32_t pages = (sizes[s] + FLASH_PAGE_SIZE - 1u) / FLASH_PAGE_SIZE;
        uint32_t expected = 1u;
        for (uint32_t p = 0; p < pages; p++) {
            uint32_t len = (p + 1u) * FLASH_PAGE_SIZE <= sizes[s] ? FLASH_PAGE_SIZE : sizes[s] - p * FLASH_PAGE_SIZE;
            expected += (len + 7u) / 8u;
        }
        CHECK((uint32_t)count_frames(OFS_BACKUP_READ_RESPONSE) == expected, "one length frame and one frame per eight bytes of each page");
        CHECK(frames <= expected, "a page boundary never makes fewer frames than the whole image needs");
        CHECK(rx_page == pages, "every page was acknowledged once");
    }
}

static void test_the_last_frame_of_a_page_carries_only_what_is_left(void)
{
    printf("  a size that is not a multiple of eight ends with a short frame\n");
    reset_world();
    install_app(13, 3);
    rx_mode = RX_ACK;
    HandleReadbackStart();
    int last = -1;
    for (int i = 0; i < sent_count; i++) {
        if (sent[i].id == SLOT_BASE + OFS_BACKUP_READ_RESPONSE) last = i;
    }
    CHECK(last >= 0 && sent[last].dlc == 5, "13 bytes end with a frame of 5");
}

static void test_the_readback_stops_when_the_host_does_not_acknowledge(void)
{
    printf("  without the acknowledgement of a page the read-back stops after the first page and three seconds\n");
    static const int modes[] = {RX_NONE, RX_WRONG_ID, RX_WRONG_PAGE, RX_WRONG_LENGTH, RX_READ_ERROR};
    for (unsigned m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
        reset_world();
        install_app(5000, 9);
        rx_mode = modes[m];
        uint32_t start = clock_ms;
        HandleReadbackStart();
        CHECK(count_frames(OFS_BACKUP_READ_RESPONSE) == 1 + FLASH_PAGE_SIZE / 8, "the length and the first page were sent, and nothing after");
        CHECK(clock_ms - start >= 3000, "it waited the whole three seconds for the acknowledgement");
        CHECK(clock_ms - start < 3000 + 10, "and not much longer");
    }
}

static void test_the_readback_serves_nothing_without_a_valid_installed_record(void)
{
    printf("  the read-back answers only with the length 0 when there is no valid installed record\n");
    FirmwareMetadata_t m;

    reset_world();                                   /* no record at all */
    rx_mode = RX_ACK;
    HandleReadbackStart();
    CHECK(count_frames(OFS_BACKUP_READ_RESPONSE) == 1 && announced_length() == 0, "no metadata: the length 0");

    reset_world();
    lay_out_app(main_slot(), 3000, 5);
    m = record_for(MAIN_APP_ADDR, META_STATE_COPY_PENDING, THIS_HARDWARE_ID, 3000);
    write_record(&m);
    rx_mode = RX_ACK;
    HandleReadbackStart();
    CHECK(count_frames(OFS_BACKUP_READ_RESPONSE) == 1 && announced_length() == 0, "a copy still pending: the length 0");

    reset_world();
    lay_out_app(main_slot(), 3000, 5);
    m = record_for(MAIN_APP_ADDR, META_STATE_APP_VALID, THIS_HARDWARE_ID + 1, 3000);
    write_record(&m);
    rx_mode = RX_ACK;
    HandleReadbackStart();
    CHECK(count_frames(OFS_BACKUP_READ_RESPONSE) == 1 && announced_length() == 0, "a record of another board: the length 0");

    reset_world();
    lay_out_app(main_slot(), 3000, 5);
    m = record_for(MAIN_APP_ADDR, META_STATE_APP_VALID, THIS_HARDWARE_ID, 3000);
    m.size = 0;
    write_record(&m);
    rx_mode = RX_ACK;
    HandleReadbackStart();
    CHECK(count_frames(OFS_BACKUP_READ_RESPONSE) == 1 && announced_length() == 0, "a size of 0: the length 0");

    reset_world();
    lay_out_app(main_slot(), 3000, 5);
    m = record_for(MAIN_APP_ADDR, META_STATE_APP_VALID, THIS_HARDWARE_ID, 3000);
    m.size = APP_MAX_SIZE + 1u;
    write_record(&m);
    rx_mode = RX_ACK;
    HandleReadbackStart();
    CHECK(count_frames(OFS_BACKUP_READ_RESPONSE) == 1 && announced_length() == 0, "a size over the slot: the length 0, never a read past the end");

    reset_world();
    lay_out_app(main_slot(), 3000, 5);
    m = record_for(MAIN_APP_ADDR, META_STATE_APP_VALID, THIS_HARDWARE_ID, 3000);
    m.size = APP_MAX_SIZE;                           /* the largest size that fits is served */
    write_record(&m);
    rx_mode = RX_ACK;
    HandleReadbackStart();
    CHECK(announced_length() == APP_MAX_SIZE, "the largest size that fits is announced");
}

static void test_the_readback_does_nothing_during_an_update(void)
{
    printf("  during an update the read-back does not answer at all\n");
    reset_world();
    install_app(3000, 5);
    update_in_progress = 1;
    rx_mode = RX_ACK;
    HandleReadbackStart();
    CHECK(sent_count == 0, "no frame");
    update_in_progress = 0;
}

static void test_the_readout_protection_gate(void)
{
    printf("  the read-out protection gate (HYDRA_READBACK_GATE=%d)\n", HYDRA_READBACK_GATE);
    const uint32_t levels[] = {RDP_LEVEL0, RDP_LEVEL1, RDP_LEVEL2};
    for (unsigned i = 0; i < 3; i++) {
        reset_world();
        install_app(3000, 5);
        host_flash_registers.OPTR = FLASH_OPTR_DBANK | levels[i];
        rx_mode = RX_ACK;
        HandleReadbackStart();
        int served = announced_length() == 3000;
        if (HYDRA_READBACK_GATE && levels[i] != RDP_LEVEL0) {
            CHECK(!served, "with the protection on and the gate on the application is not served");
            CHECK(count_frames(OFS_BACKUP_READ_RESPONSE) == 1 && announced_length() == 0, "the answer is the length 0");
        } else {
            CHECK(served, "the application is served (protection off, or the gate off as in the earlier firmware)");
        }
    }
}

/* ---- ApplicationIsValid --------------------------------------------------------------------------------------------------- */
static void test_an_application_without_a_record_is_adopted_only_with_a_plausible_stack(void)
{
    printf("  an application with no record yet is adopted when its stack pointer is plausible, refused when it is not\n");
    const uint32_t refused[] = {0xFFFFFFFFUL, 0x00000000UL, SRAM_BASE + 0xFFUL, SRAM_BASE + 0x18001UL, 0x08000000UL};
    for (unsigned i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        reset_world();
        put_le32(main_slot(), refused[i]);
        CHECK(ApplicationIsValid() == 0, "an implausible stack pointer is not adopted");
        FirmwareMetadata_t m;
        CHECK(Metadata_Read(&m) == 0, "and no record is written for it");
    }
    const uint32_t accepted[] = {SRAM_BASE + 0x100UL, SRAM_BASE + 0x18000UL, 0x20010000UL};
    for (unsigned i = 0; i < sizeof(accepted) / sizeof(accepted[0]); i++) {
        reset_world();
        put_le32(main_slot(), accepted[i]);
        CHECK(ApplicationIsValid() == 1, "a plausible stack pointer is adopted (both ends of the range count)");
        FirmwareMetadata_t m = metadata();
        CHECK(m.state == META_STATE_APP_VALID && m.hardware_id == THIS_HARDWARE_ID, "the adopted record is valid and of this board");
        CHECK(m.size == APP_MAX_SIZE, "it covers the whole slot, as the size is not known");
        FirmwareMetadata_t again = record_for(MAIN_APP_ADDR, META_STATE_APP_VALID, THIS_HARDWARE_ID, APP_MAX_SIZE);
        CHECK(m.crc32 == again.crc32 && memcmp(m.sha256, again.sha256, 32) == 0, "its CRC32 and SHA-256 are those of the flash");
        CHECK(ApplicationIsValid() == 1, "and the next start finds it valid by the record");
    }
}

static void test_an_adoption_that_does_not_take_is_a_refusal(void)
{
    printf("  a flash write that does not take makes the adoption fail instead of starting from other metadata\n");
    reset_world();
    put_le32(main_slot(), 0x20010000UL);
    fail_program = 1;
    CHECK(ApplicationIsValid() == 0, "no adoption when the record cannot be written");
    fail_program = 0;
    FirmwareMetadata_t m;
    CHECK(Metadata_Read(&m) == 0, "and nothing was recorded");
}

static void test_a_record_that_matches_the_flash_is_valid(void)
{
    printf("  an installed application is valid while the flash matches its record, and not when it does not\n");
    reset_world();
    install_app(4000, 11);
    CHECK(ApplicationIsValid() == 1, "the record matches the flash");

    reset_world();
    install_app(4000, 11);
    main_slot()[1234] ^= 0x01;
    CHECK(ApplicationIsValid() == 0, "one flipped bit breaks the CRC");

    reset_world();
    install_app(4000, 11);
    main_slot()[3999] ^= 0x80;
    CHECK(ApplicationIsValid() == 0, "the last byte is covered too");

    reset_world();
    install_app(4000, 11);
    main_slot()[4000] = 0x00;                         /* a byte past the end of the image does not matter */
    CHECK(ApplicationIsValid() == 1, "a byte past the declared size is not part of the image");
}

static void test_a_record_with_a_wrong_digest_is_refused_even_with_a_right_crc(void)
{
    printf("  the SHA-256 of the record is checked as well as the CRC32\n");
    reset_world();
    lay_out_app(main_slot(), 4000, 11);
    FirmwareMetadata_t m = record_for(MAIN_APP_ADDR, META_STATE_APP_VALID, THIS_HARDWARE_ID, 4000);
    m.sha256[0] ^= 0xFF;
    write_record(&m);
    CHECK(ApplicationIsValid() == 0, "a digest that differs in one byte is refused");

    reset_world();
    lay_out_app(main_slot(), 4000, 11);
    m = record_for(MAIN_APP_ADDR, META_STATE_APP_VALID, THIS_HARDWARE_ID, 4000);
    m.sha256[31] ^= 0x01;
    write_record(&m);
    CHECK(ApplicationIsValid() == 0, "also in the last byte");
}

static void test_a_record_of_another_board_or_with_an_impossible_size_is_refused(void)
{
    printf("  a record of another board, or with a size of 0 or over the slot, is refused\n");
    FirmwareMetadata_t m;

    reset_world();
    lay_out_app(main_slot(), 4000, 11);
    m = record_for(MAIN_APP_ADDR, META_STATE_APP_VALID, THIS_HARDWARE_ID ^ 1u, 4000);
    write_record(&m);
    CHECK(ApplicationIsValid() == 0, "another hardware id");

    reset_world();
    lay_out_app(main_slot(), 4000, 11);
    m = record_for(MAIN_APP_ADDR, META_STATE_APP_VALID, THIS_HARDWARE_ID, 4000);
    m.size = 0;
    write_record(&m);
    CHECK(ApplicationIsValid() == 0, "a size of 0");

    reset_world();
    lay_out_app(main_slot(), 4000, 11);
    m = record_for(MAIN_APP_ADDR, META_STATE_APP_VALID, THIS_HARDWARE_ID, 4000);
    m.size = APP_MAX_SIZE + 1u;
    write_record(&m);
    CHECK(ApplicationIsValid() == 0, "a size over the slot");
}

static void test_a_pending_copy_is_finished_at_boot(void)
{
    printf("  a record that asks for a copy copies the backup into the main slot and turns valid\n");
    reset_world();
    lay_out_app(backup_slot(), 5000, 13);
    FirmwareMetadata_t m = record_for(BACKUP_APP_ADDR, META_STATE_COPY_PENDING, THIS_HARDWARE_ID, 5000);
    write_record(&m);
    CHECK(ApplicationIsValid() == 1, "the copy is made and the application is valid");
    CHECK(memcmp(main_slot(), backup_slot(), 5000) == 0, "the main slot holds the backup");
    CHECK(metadata().state == META_STATE_APP_VALID, "the record turned valid");
    int saw_copying = 0;
    for (int i = 0; i < sent_count; i++) {
        if (sent[i].id == SLOT_BASE + OFS_STATUS && sent[i].data[0] == STATUS_COPYING) saw_copying = 1;
    }
    CHECK(saw_copying, "the host was told that the copy was running");
    CHECK(count_frames(OFS_HEARTBEAT) >= 3, "and a heartbeat went out for each page");

    reset_world();
    lay_out_app(backup_slot(), 5000, 13);
    m = record_for(BACKUP_APP_ADDR, META_STATE_COPY_PENDING, THIS_HARDWARE_ID, 5000);
    write_record(&m);
    fail_erase = 1;
    CHECK(ApplicationIsValid() == 0, "a copy whose erase fails is not valid");
    CHECK(metadata().state == META_STATE_COPY_PENDING, "and the record still asks for the copy");

    reset_world();
    lay_out_app(backup_slot(), 5000, 13);
    m = record_for(BACKUP_APP_ADDR, META_STATE_COPY_PENDING, THIS_HARDWARE_ID, 5000);
    m.crc32 ^= 1u;
    write_record(&m);
    CHECK(ApplicationIsValid() == 0, "a copy whose CRC does not match the record is not valid, even after the copy");
}

/* ---- the answers to the queries ------------------------------------------------------------------------------------------- */
static void test_the_version_query(void)
{
    printf("  the version query answers with this bootloader and the installed application\n");
    reset_world();
    HandleVersionQuery();
    CHECK(sent_count == 2, "two frames");
    CHECK(sent[0].id == SLOT_BASE + OFS_VERSION_RESPONSE && sent[0].dlc == 8 && sent[0].data[0] == 1, "the first says it is the bootloader answering");
    CHECK(sent[0].data[1] == 0 && sent[0].data[2] == 0 && sent[0].data[3] == 0 && sent[0].data[4] == 0 && sent[0].data[7] == 0, "without a record the hardware id and the version are 0");
    CHECK(sent[1].id == SLOT_BASE + OFS_BOOTLOADER_VERSION && sent[1].dlc == 3, "the second carries the version of the bootloader");
    CHECK(sent[1].data[0] == BOOTLOADER_VERSION_MAJOR && sent[1].data[1] == BOOTLOADER_VERSION_MINOR && sent[1].data[2] == BOOTLOADER_VERSION_PATCH, "the bytes are the compiled version");

    reset_world();
    install_app(2000, 3);
    FirmwareMetadata_t m = metadata();
    m.version_major = 0x0102;
    m.version_minor = 0x37;
    write_record(&m);
    HandleVersionQuery();
    uint32_t hw = ((uint32_t)sent[0].data[1] << 24) | ((uint32_t)sent[0].data[2] << 16) | ((uint32_t)sent[0].data[3] << 8) | sent[0].data[4];
    CHECK(hw == THIS_HARDWARE_ID, "the hardware id of the record");
    CHECK(sent[0].data[5] == 0x01 && sent[0].data[6] == 0x02 && sent[0].data[7] == 0x37, "the major in two bytes and the minor in one");
}

static void test_the_error_counter_query(void)
{
    printf("  the error-counter query reads the transmit and receive counters of the controller\n");
    reset_world();
    fdcan_registers.ECR = (0x45u << 8) | 0x2Au;
    HandleErrorCounterQuery();
    CHECK(sent_count == 1 && sent[0].id == SLOT_BASE + OFS_ERROR_COUNTERS_RESPONSE && sent[0].dlc == 2, "one frame of two bytes");
    CHECK(sent[0].data[0] == 0x2A && sent[0].data[1] == 0x45, "transmit first, receive second");

    reset_world();
    fdcan_registers.ECR = 0xFFFFu;
    HandleErrorCounterQuery();
    CHECK(sent[0].data[0] == 0xFF && sent[0].data[1] == 0x7F, "the receive counter is seven bits: the passive flag is not part of it");
}

static void test_the_small_senders_and_the_slot_base(void)
{
    printf("  status, heartbeat and scan reply go out on this board's own window\n");
    reset_world();
    CAN_SendStatus(STATUS_LISTENING);
    CHECK(sent_count == 1 && sent[0].id == SLOT_BASE + OFS_STATUS && sent[0].dlc == 1 && sent[0].data[0] == STATUS_LISTENING, "status");

    reset_world();
    CAN_SendHeartbeat(STATUS_RECEIVING, 42);
    CHECK(sent_count == 1 && sent[0].id == SLOT_BASE + OFS_HEARTBEAT && sent[0].dlc == 2 && sent[0].data[0] == STATUS_RECEIVING && sent[0].data[1] == 42, "heartbeat");

    reset_world();
    const uint8_t probe[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    CAN_SendSlotScanReply(probe);
    CHECK(sent_count == 1 && sent[0].id == SLOT_BASE + OFS_SLOT_SCAN_REPLY && sent[0].dlc == 8 && memcmp(sent[0].data, probe, 8) == 0, "scan reply");

    reset_world();
    Protocol_Init(0x640UL);
    CAN_SendStatus(STATUS_ERROR);
    CHECK(sent_count == 1 && sent[0].id == 0x640UL + OFS_STATUS, "another slot base moves every identifier");
    Protocol_Init(SLOT_BASE);
}

static void test_a_full_transmit_fifo_drops_or_delays_but_never_blocks(void)
{
    printf("  a transmit FIFO that stays full drops the frame after fifty milliseconds; one that frees up in time sends it\n");
    reset_world();
    tx_busy_polls = 1000000;
    uint32_t start = clock_ms;
    CAN_SendStatus(STATUS_LISTENING);
    CHECK(sent_count == 0, "a frame that finds no mailbox is dropped");
    CHECK(clock_ms - start == 50, "after fifty polls of one millisecond");

    reset_world();
    tx_busy_polls = 10;
    start = clock_ms;
    CAN_SendStatus(STATUS_LISTENING);
    CHECK(sent_count == 1, "a mailbox that frees up after ten polls takes the frame");
    CHECK(clock_ms - start == 10, "and it waited ten milliseconds");

    reset_world();
    tx_busy_polls = 3;
    CAN_SendHeartbeat(STATUS_RECEIVING, 1);
    CHECK(sent_count == 0 && clock_ms == 1000, "a heartbeat never waits: it is skipped when the FIFO is full");
}

/* ---- JumpToApplication ------------------------------------------------------------------------------------------------------ */
static void test_the_jump_refuses_what_is_not_an_application(void)
{
    printf("  the jump leaves the bootloader untouched when the vectors are not those of an application\n");
    struct { uint32_t sp, reset; const char *why; } cases[] = {
        {0x00000000UL, MAIN_APP_ADDR + 0x101UL, "a stack pointer outside the RAM"},
        {0x20010002UL, MAIN_APP_ADDR + 0x101UL, "a stack pointer that is not aligned to four"},
        {0x20010000UL, MAIN_APP_ADDR + 0x100UL, "a reset vector without the Thumb bit"},
        {0x20010000UL, MAIN_APP_ADDR - 3UL, "a reset vector before the slot"},
        {0x20010000UL, MAIN_APP_ADDR + APP_MAX_SIZE + 1UL, "a reset vector after the slot"},
        {0xFFFFFFFFUL, 0xFFFFFFFFUL, "an erased slot"},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        reset_world();
        put_le32(main_slot(), cases[i].sp);
        put_le32(main_slot() + 4, cases[i].reset);
        JumpToApplication();
        if (deinit_calls != 0 || host_scb.VTOR != 0) {
            printf("  FAIL: %s still shut the bootloader down\n", cases[i].why);
            failures++;
        }
    }
}

int main(void)
{
    printf("HYDRA-UMC host tests - G474 bootloader, read-back, validity at boot and queries (read-out gate %d)\n", HYDRA_READBACK_GATE);

    flash_mem = mmap((void *)FLASH_BASE_ADDR, FLASH_TOTAL, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (flash_mem == MAP_FAILED || (uintptr_t)flash_mem != FLASH_BASE_ADDR) {
        printf("  SKIP: cannot map the flash address range on this host\n");
        return 0;
    }

    test_the_readback_serves_every_page_of_the_installed_application();
    test_the_last_frame_of_a_page_carries_only_what_is_left();
    test_the_readback_stops_when_the_host_does_not_acknowledge();
    test_the_readback_serves_nothing_without_a_valid_installed_record();
    test_the_readback_does_nothing_during_an_update();
    test_the_readout_protection_gate();
    test_an_application_without_a_record_is_adopted_only_with_a_plausible_stack();
    test_an_adoption_that_does_not_take_is_a_refusal();
    test_a_record_that_matches_the_flash_is_valid();
    test_a_record_with_a_wrong_digest_is_refused_even_with_a_right_crc();
    test_a_record_of_another_board_or_with_an_impossible_size_is_refused();
    test_a_pending_copy_is_finished_at_boot();
    test_the_version_query();
    test_the_error_counter_query();
    test_the_small_senders_and_the_slot_base();
    test_a_full_transmit_fifo_drops_or_delays_but_never_blocks();
    test_the_jump_refuses_what_is_not_an_application();

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all G474 bootloader read-back and query host tests passed\n");
    return 0;
}

#else /* not Linux on x86-64 */

int main(void)
{
    printf("HYDRA-UMC host tests - G474 bootloader read-back: skipped (needs Linux on x86-64)\n");
    return 0;
}

#endif
