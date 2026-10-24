/* =============================================================================
 * HYDRA-UMC Firmware - Host tests of the G474 bootloader's whole update, from START_UPDATE to the new application
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * bootloader_protocol.c is compiled as it is against tests/stubs/protocol/stm32g4xx_hal.h; this file plays the host that updates the
 * board (it sends the frames HandleStartUpdate, HandleHmacChunk, HandleImageHeaderChunk, HandleDowngradeTokenChunk, HandleData and
 * HandleEndUpdate receive), the CAN bus (it records every frame the bootloader sends), the flash controller and the clock. Nothing runs on a board.
 *
 *  - the signed header and the anti-rollback rule, end to end: an update over the installed version, the old flaws (a replayed image that
 *    claims a higher version, 0.1.8 -> 0.1.2 that only compared major.minor) and the downgrade that needs a token;
 *  - the downgrade token: bound to this session's nonce, to the pair of versions and to the image; the old constant-magic frame does nothing;
 *  - the tag over header || image, with the key of the bootloader, against bytes the host reference (tools/image_header.py) made;
 *  - everything that must leave the main slot and the installed metadata untouched when an update is refused.
 *
 * Needs Linux on x86-64 (a fixed mapping at the flash address); anywhere else it reports that and passes.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__) && defined(__x86_64__)
#include <sys/mman.h>

#include "image_header_vectors.h"

/* The key of these tests is the one of the vectors, so the reference's tag verifies under the bootloader's own key. */
#define HYDRA_FW_HMAC_KEY_BYTES \
    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F, \
    0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F

#include "stm32g4xx_hal.h"

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

uint32_t HAL_GetTick(void) { return clock_ms; }
void HAL_Delay(uint32_t ms) { clock_ms += ms; }
HAL_StatusTypeDef HAL_IWDG_Refresh(IWDG_HandleTypeDef *h) { (void)h; return HAL_OK; }
void HAL_NVIC_DisableIRQ(int irq) { (void)irq; }
HAL_StatusTypeDef HAL_FDCAN_DeInit(FDCAN_HandleTypeDef *h) { (void)h; return HAL_OK; }
void HAL_RCC_DeInit(void) {}
void HAL_DeInit(void) {}
void __disable_irq(void) {}
void __set_MSP(uint32_t top) { (void)top; }
void NVIC_SystemReset(void) { resets++; }

/* ---- the CAN bus: every frame the bootloader sends ------------------------------------------------------------------------------- */
typedef struct { uint32_t id; uint32_t dlc; uint8_t data[8]; } Frame;
static Frame sent[4096];
static int sent_count;

uint32_t HAL_FDCAN_GetTxFifoFreeLevel(FDCAN_HandleTypeDef *h) { (void)h; return 1; }
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
uint32_t HAL_FDCAN_GetRxFifoFillLevel(FDCAN_HandleTypeDef *h, uint32_t fifo) { (void)h; (void)fifo; return 0; }
HAL_StatusTypeDef HAL_FDCAN_GetRxMessage(FDCAN_HandleTypeDef *h, uint32_t fifo, FDCAN_RxHeaderTypeDef *header, uint8_t *data)
{
    (void)h; (void)fifo; (void)header; (void)data;
    return HAL_ERROR;
}

/* ---- the flash controller -------------------------------------------------------------------------------------------------------- */
#define FLASH_BASE_ADDR  0x08000000UL
#define FLASH_TOTAL      0x00080000UL
#define BANK_BYTES       0x00040000UL
#define SCRATCH_ADDR     0x08070000UL  /* where the test lays an image out to sign it; far from the slots */

static uint8_t *flash_mem;
static int is_unlocked;

HAL_StatusTypeDef HAL_FLASH_Unlock(void) { is_unlocked = 1; return HAL_OK; }
HAL_StatusTypeDef HAL_FLASH_Lock(void) { is_unlocked = 0; return HAL_OK; }

HAL_StatusTypeDef HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef *init, uint32_t *page_error)
{
    *page_error = 0xFFFFFFFFUL;
    if (!is_unlocked || init->TypeErase != FLASH_TYPEERASE_PAGES || init->NbPages != 1U) return HAL_ERROR;
    if ((init->Banks != FLASH_BANK_1 && init->Banks != FLASH_BANK_2) || init->Page > 127U) return HAL_ERROR;
    uint32_t offset = (init->Banks == FLASH_BANK_2 ? BANK_BYTES : 0UL) + init->Page * FLASH_PAGE_SIZE;
    memset(flash_mem + offset, 0xFF, FLASH_PAGE_SIZE);
    return HAL_OK;
}

HAL_StatusTypeDef HAL_FLASH_Program(uint32_t type, uint32_t address, uint64_t data)
{
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
#include "bootloader_protocol.c"

/* ---- the host side of an update ------------------------------------------------------------------------------------------------- */
#define SLOT_BASE 0x600UL

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static uint32_t crc_of(const uint8_t *data, uint32_t len)
{
    return CRC32_Finalize(CRC32_Update(0xFFFFFFFFUL, data, len));
}

/* An update as the host holds it before sending it. */
typedef struct {
    uint8_t  header[IMAGE_HEADER_SIZE];
    uint8_t *image;
    uint32_t size;
    uint8_t  tag[IMAGE_TAG_SIZE];
    uint8_t  token[IMAGE_TAG_SIZE];
    int      token_chunks;      /* how many of the four to send */
    int      header_chunks;     /* how many of the eight to send */
    int      tag_chunks;        /* how many of the four to send */
    uint32_t declared_crc;
    uint16_t declared_major, declared_minor;
} Update;

static void put_le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_le32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static void build_header(uint8_t header[IMAGE_HEADER_SIZE], uint32_t hw_id, ImageVersion_t version, uint32_t size, uint32_t link,
                         uint32_t sp, uint32_t reset, uint32_t crc)
{
    memset(header, 0, IMAGE_HEADER_SIZE);
    put_le32(header + 0, IMAGE_HEADER_MAGIC);
    put_le16(header + 4, IMAGE_HEADER_FORMAT);
    put_le32(header + 8, hw_id);
    put_le16(header + 12, version.major);
    put_le16(header + 14, version.minor);
    put_le16(header + 16, version.patch);
    put_le32(header + 20, size);
    put_le32(header + 24, link);
    put_le32(header + 28, sp);
    put_le32(header + 32, reset);
    put_le32(header + 36, crc);
}

/* The tag of header || image under the key of the bootloader, with the routine under test. */
static void sign(Update *u)
{
    memcpy(flash_mem + (SCRATCH_ADDR - FLASH_BASE_ADDR), u->image, u->size);
    hmac_sha256_prefixed_flash_region(HMAC_KEY, 32, u->header, IMAGE_HEADER_SIZE, SCRATCH_ADDR, u->size, u->tag);
}

/* A consistent update: an image whose first two words are the vectors the header declares, signed. */
static Update make_update(ImageVersion_t version, uint32_t seed, uint32_t size)
{
    Update u;
    memset(&u, 0, sizeof(u));
    u.size = size;
    u.image = calloc(1, size);
    for (uint32_t i = 0; i < size; i++) u.image[i] = (uint8_t)((i * seed + 3) & 0xFF);
    uint32_t sp = 0x20010000UL, reset = MAIN_APP_ADDR + 0x101UL;
    put_le32(u.image, sp);
    put_le32(u.image + 4, reset);
    u.declared_crc = crc_of(u.image, size);
    build_header(u.header, THIS_HARDWARE_ID, version, size, MAIN_APP_ADDR, sp, reset, u.declared_crc);
    sign(&u);
    u.token_chunks = 0;
    u.header_chunks = 8;
    u.tag_chunks = 4;
    u.declared_major = version.major;
    u.declared_minor = version.minor;
    return u;
}

static void free_update(Update *u) { free(u->image); u->image = NULL; }

static void reset_world(void)
{
    memset(flash_mem, 0xFF, FLASH_TOTAL);
    host_flash_registers.OPTR = FLASH_OPTR_DBANK;
    is_unlocked = 0;
    sent_count = 0;
    resets = 0;
    clock_ms = 1000;
    memset(&update_header, 0, sizeof(update_header));
    update_in_progress = 0;
    update_failed = 0;
    hfdcan1.Instance = &fdcan_registers;
    Protocol_Init(SLOT_BASE);
}

/* What is installed: the metadata page, and a main slot that holds a recognizable pattern. */
static void install(uint32_t major, uint32_t minor, uint32_t patch_field)
{
    FirmwareMetadata_t m;
    memset(&m, 0, sizeof(m));
    m.magic = METADATA_MAGIC_VALID;
    m.state = META_STATE_APP_VALID;
    m.hardware_id = THIS_HARDWARE_ID;
    m.version_major = major;
    m.version_minor = minor;
    m.version_patch = patch_field;
    m.size = 1024;
    m.crc32 = 0x12345678UL;
    memset(m.hmac, 0x77, sizeof(m.hmac));
    memcpy(flash_mem + (METADATA_ADDR - FLASH_BASE_ADDR), &m, sizeof(m));
    memset(flash_mem + (MAIN_APP_ADDR - FLASH_BASE_ADDR), 0xA5, 4096);
}

static FirmwareMetadata_t metadata(void)
{
    FirmwareMetadata_t m;
    memcpy(&m, flash_mem + (METADATA_ADDR - FLASH_BASE_ADDR), sizeof(m));
    return m;
}

static int main_slot_untouched(void)
{
    for (uint32_t i = 0; i < 4096; i++) {
        if (flash_mem[MAIN_APP_ADDR - FLASH_BASE_ADDR + i] != 0xA5) return 0;
    }
    return 1;
}

static int find_frames(uint32_t offset, Frame *out, int max)
{
    int n = 0;
    for (int i = 0; i < sent_count; i++) {
        if (sent[i].id == SLOT_BASE + offset && n < max) out[n++] = sent[i];
    }
    return n;
}

/* The last verdict the bootloader sent: 0 = verify OK, otherwise the reason of the failure; -1 = none. */
static int last_verdict(void)
{
    for (int i = sent_count - 1; i >= 0; i--) {
        if (sent[i].id != SLOT_BASE + OFS_STATUS) continue;
        if (sent[i].data[0] == STATUS_VERIFY_OK) return 0;
        if (sent[i].data[0] == STATUS_VERIFY_FAIL) return sent[i].data[1];
        if (sent[i].data[0] == STATUS_ERROR) return 0xFF;
    }
    return -1;
}

/* The nonce of the session just started: the host asks for each half (OFS_DOWNGRADE_NONCE_REQUEST) and the bootloader answers it. */
static int announced_nonce(uint8_t nonce[IMAGE_NONCE_SIZE])
{
    for (uint8_t half = 0; half < 2; half++) {
        int before = sent_count;
        uint8_t request = half;
        HandleDowngradeNonceRequest(&request, 1);
        if (sent_count != before + 1 || sent[sent_count - 1].id != SLOT_BASE + OFS_DOWNGRADE_NONCE_RESPONSE) return 0;
        memcpy(nonce + 8 * half, sent[sent_count - 1].data, 8);
    }
    return 1;
}

static void start(const Update *u)
{
    uint8_t d[8];
    put_be32(d, u->size);
    put_be32(d + 4, THIS_HARDWARE_ID);
    HandleStartUpdate(d);
}

/* Everything between START_UPDATE and END_UPDATE; `after_start` lets a test compute a token for the nonce it was told. */
static void send_body(Update *u)
{
    for (int i = 0; i < u->tag_chunks; i++) HandleHmacChunk(u->tag + 8 * i);
    for (int i = 0; i < u->header_chunks; i++) HandleImageHeaderChunk(u->header + 8 * i);
    for (int i = 0; i < u->token_chunks; i++) HandleDowngradeTokenChunk(u->token + 8 * i);
    for (uint32_t at = 0; at < u->size; at += 8) {
        uint32_t n = u->size - at < 8 ? u->size - at : 8;
        HandleData(u->image + at, n);
    }
}

static void finish(const Update *u)
{
    uint8_t d[8];
    put_be32(d, u->declared_crc);
    d[4] = (uint8_t)(u->declared_major >> 8); d[5] = (uint8_t)u->declared_major;
    d[6] = (uint8_t)(u->declared_minor >> 8); d[7] = (uint8_t)u->declared_minor;
    HandleEndUpdate(d);
}

/* The whole update with the token (if any) computed for the nonce the bootloader announced. */
static int run_update(Update *u, ImageVersion_t token_installed, ImageVersion_t token_target, int with_token)
{
    start(u);
    if (with_token) {
        uint8_t nonce[IMAGE_NONCE_SIZE];
        if (!announced_nonce(nonce)) return -2;
        uint8_t message[DOWNGRADE_MESSAGE_LEN];
        ImageHeader_DowngradeMessage(nonce, &token_installed, &token_target, u->tag, message);
        hmac_sha256_buffer(HMAC_KEY, 32, message, sizeof(message), u->token);
        u->token_chunks = 4;
    }
    send_body(u);
    finish(u);
    return last_verdict();
}

static int main_slot_holds(const Update *u)
{
    return memcmp(flash_mem + (MAIN_APP_ADDR - FLASH_BASE_ADDR), u->image, u->size) == 0;
}

/* ---- tests ----------------------------------------------------------------------------------------------------------------------- */
static void test_the_reference_image_installs_over_the_installed_version(void)
{
    printf("  an update over the installed version\n");
    reset_world();
    install(0, 2, 2);
    Update u;
    memset(&u, 0, sizeof(u));
    /* the exact bytes the host reference made: header, image and tag of vectors image A */
    u.size = VEC_IMAGE_SIZE;
    u.image = (uint8_t *)VEC_IMAGE;
    memcpy(u.header, VEC_HEADER, sizeof(u.header));
    memcpy(u.tag, VEC_TAG, sizeof(u.tag));
    u.declared_crc = VEC_IMAGE_CRC32;
    u.declared_major = VEC_VERSION_MAJOR;
    u.declared_minor = VEC_VERSION_MINOR;
    u.header_chunks = 8;
    u.tag_chunks = 4;
    ImageVersion_t none = {0, 0, 0};
    CHECK(run_update(&u, none, none, 0) == 0, "the tag the reference made is accepted by the bootloader");
    CHECK(resets == 1, "the board resets into the new application");
    CHECK(main_slot_holds(&u), "the image is in the main slot");
    FirmwareMetadata_t m = metadata();
    CHECK(m.state == META_STATE_APP_VALID && m.version_major == 0 && m.version_minor == 2 && m.version_patch == 3, "the metadata holds 0.2.3, the patch included");
    CHECK(m.size == VEC_IMAGE_SIZE && m.crc32 == VEC_IMAGE_CRC32, "size and CRC");
    uint8_t expected[32];
    hmac_sha256_flash_region(HMAC_KEY, 32, MAIN_APP_ADDR, VEC_IMAGE_SIZE, expected);
    CHECK(memcmp(m.hmac, expected, 32) == 0, "the boot-time check value is the HMAC of the image as it sits in the main slot");
}

static void test_the_session_nonce_is_asked_for_and_changes(void)
{
    printf("  the nonce of the session\n");
    reset_world();
    install(0, 2, 2);
    Update u = make_update((ImageVersion_t){0, 2, 3}, 5, 512);
    uint8_t request = 0;
    int before = sent_count;
    HandleDowngradeNonceRequest(&request, 1);
    CHECK(sent_count == before, "no answer before an update has started");
    start(&u);
    uint8_t first[IMAGE_NONCE_SIZE], second[IMAGE_NONCE_SIZE];
    CHECK(announced_nonce(first), "each half is answered with eight bytes");
    Frame answers[4];
    CHECK(find_frames(OFS_DOWNGRADE_NONCE_RESPONSE, answers, 4) == 2 && answers[0].dlc == 8 && answers[1].dlc == 8, "on the bootloader's own identifier");
    CHECK(memcmp(first, first + 8, 8) != 0, "the two halves are not the same bytes");
    start(&u);
    CHECK(announced_nonce(second) && memcmp(first, second, sizeof(first)) != 0, "another session, another nonce");
    /* a request that is not a half, or has the wrong length, is ignored */
    before = sent_count;
    request = 2;
    HandleDowngradeNonceRequest(&request, 1);
    uint8_t two[2] = {0, 0};
    HandleDowngradeNonceRequest(two, 2);
    CHECK(sent_count == before, "half 2 and a request of two bytes get no answer");
    free_update(&u);
}

static void test_a_replayed_old_image_cannot_claim_a_higher_version(void)
{
    printf("  the old flaw: a replayed image that claims a newer version\n");
    reset_world();
    install(0, 2, 2);
    Update u = make_update((ImageVersion_t){0, 1, 8}, 9, 512); /* a genuinely signed 0.1.8 */
    /* the host changes the version inside the header and in END_UPDATE: the tag no longer matches the header */
    put_le16(u.header + 12, 0);
    put_le16(u.header + 14, 9);
    put_le16(u.header + 16, 9);
    u.declared_minor = 9;
    ImageVersion_t none = {0, 0, 0};
    CHECK(run_update(&u, none, none, 0) == VERIFY_FAIL_REASON_HMAC, "the tag covers the version: changing it breaks the tag");
    CHECK(resets == 0 && main_slot_untouched(), "nothing was installed");
    CHECK(metadata().version_minor == 2 && metadata().version_patch == 2, "and the installed version is the one it was");
    free_update(&u);
}

static void test_the_patch_counts_in_the_anti_rollback(void)
{
    printf("  anti-rollback compares major.minor.patch\n");
    reset_world();
    install(0, 1, 8);
    Update u = make_update((ImageVersion_t){0, 1, 2}, 3, 512); /* 0.1.8 -> 0.1.2: the old flow compared only major.minor and let it through */
    ImageVersion_t none = {0, 0, 0};
    CHECK(run_update(&u, none, none, 0) == VERIFY_FAIL_REASON_ROLLBACK, "0.1.2 is older than the installed 0.1.8");
    CHECK(main_slot_untouched(), "main slot untouched");
    free_update(&u);

    reset_world();
    install(0, 1, 8);
    Update same = make_update((ImageVersion_t){0, 1, 8}, 4, 512);
    CHECK(run_update(&same, none, none, 0) == VERIFY_FAIL_REASON_ROLLBACK, "the same version is not newer");
    free_update(&same);

    reset_world();
    install(0, 1, 8);
    Update newer = make_update((ImageVersion_t){0, 1, 9}, 4, 512);
    CHECK(run_update(&newer, none, none, 0) == 0, "0.1.9 over 0.1.8 installs");
    CHECK(metadata().version_patch == 9, "and is recorded with its patch");
    free_update(&newer);
}

static void test_metadata_written_before_the_patch_field_reads_as_patch_zero(void)
{
    printf("  a metadata page from before the patch field\n");
    ImageVersion_t none = {0, 0, 0};
    reset_world();
    install(0, 1, 0xFFFFFFFFUL); /* erased flash where the patch will be */
    Update same = make_update((ImageVersion_t){0, 1, 0}, 6, 512);
    CHECK(run_update(&same, none, none, 0) == VERIFY_FAIL_REASON_ROLLBACK, "0.1.0 is not newer than an installed 0.1 (patch 0)");
    free_update(&same);
    reset_world();
    install(0, 1, 0xFFFFFFFFUL);
    Update newer = make_update((ImageVersion_t){0, 1, 1}, 6, 512);
    CHECK(run_update(&newer, none, none, 0) == 0, "0.1.1 is newer than it");
    free_update(&newer);
}

static void test_a_downgrade_needs_the_token_and_only_the_right_one(void)
{
    printf("  a downgrade needs a token made for this session, this pair and this image\n");
    ImageVersion_t installed = {0, 2, 2}, target = {0, 1, 8}, other_target = {0, 1, 7}, other_installed = {0, 2, 5};

    reset_world();
    install(0, 2, 2);
    Update u = make_update(target, 5, 400);
    CHECK(run_update(&u, installed, target, 0) == VERIFY_FAIL_REASON_ROLLBACK, "without a token the older image is refused");
    CHECK(main_slot_untouched(), "main slot untouched");
    free_update(&u);

    reset_world();
    install(0, 2, 2);
    u = make_update(target, 5, 400);
    CHECK(run_update(&u, installed, target, 1) == 0, "with the token made for this session, 0.2.2 -> 0.1.8 and this image");
    CHECK(main_slot_holds(&u) && metadata().version_minor == 1 && metadata().version_patch == 8, "the downgrade is installed and recorded");
    free_update(&u);

    reset_world();
    install(0, 2, 2);
    u = make_update(target, 5, 400);
    CHECK(run_update(&u, installed, other_target, 1) == VERIFY_FAIL_REASON_ROLLBACK, "a token for another target version");
    free_update(&u);

    reset_world();
    install(0, 2, 2);
    u = make_update(target, 5, 400);
    CHECK(run_update(&u, other_installed, target, 1) == VERIFY_FAIL_REASON_ROLLBACK, "a token for another installed version");
    free_update(&u);

    /* a token recorded in one session is useless in the next one */
    reset_world();
    install(0, 2, 2);
    u = make_update(target, 5, 400);
    start(&u);
    uint8_t old_nonce[IMAGE_NONCE_SIZE];
    announced_nonce(old_nonce);
    uint8_t message[DOWNGRADE_MESSAGE_LEN];
    ImageHeader_DowngradeMessage(old_nonce, &installed, &target, u.tag, message);
    uint8_t recorded[32];
    hmac_sha256_buffer(HMAC_KEY, 32, message, sizeof(message), recorded);
    HandleEndUpdate((uint8_t[8]){0});   /* the first session ends without installing anything */
    reset_world();
    install(0, 2, 2);
    sent_count = 0;
    start(&u);                          /* a new session: a new nonce */
    memcpy(u.token, recorded, 32);
    u.token_chunks = 4;
    send_body(&u);
    finish(&u);
    CHECK(last_verdict() == VERIFY_FAIL_REASON_ROLLBACK, "the token of an earlier session does not work in another");
    free_update(&u);

    /* a token is for one image: made for the tag of image u, offered with another image of the same version in a session of its own */
    reset_world();
    install(0, 2, 2);
    Update fresh = make_update(target, 11, 400);
    start(&fresh);
    uint8_t nonce[IMAGE_NONCE_SIZE];
    announced_nonce(nonce);
    u = make_update(target, 5, 400);
    ImageHeader_DowngradeMessage(nonce, &installed, &target, u.tag, message);
    hmac_sha256_buffer(HMAC_KEY, 32, message, sizeof(message), fresh.token);
    fresh.token_chunks = 4;
    send_body(&fresh);
    finish(&fresh);
    CHECK(last_verdict() == VERIFY_FAIL_REASON_ROLLBACK, "a token made for another image of the same version does not open this one");
    free_update(&u);
    free_update(&fresh);
}

static void test_the_old_constant_frame_no_longer_allows_a_downgrade(void)
{
    printf("  the old four-byte magic that switched the anti-rollback off\n");
    reset_world();
    install(0, 2, 2);
    Update u = make_update((ImageVersion_t){0, 1, 8}, 5, 300);
    start(&u);
    uint8_t magic[4] = {0xD0, 0x9E, 0x12, 0xAD};
    HandleAuthorizeDowngrade(magic);
    send_body(&u);
    finish(&u);
    CHECK(last_verdict() == VERIFY_FAIL_REASON_ROLLBACK, "the frame anybody on the bus could send does nothing");
    CHECK(main_slot_untouched(), "main slot untouched");
    free_update(&u);
}

static void test_an_update_that_did_not_arrive_whole_is_refused(void)
{
    printf("  incomplete updates\n");
    ImageVersion_t none = {0, 0, 0};
    reset_world();
    install(0, 2, 2);
    Update u = make_update((ImageVersion_t){0, 2, 3}, 5, 512);
    u.header_chunks = 7;
    CHECK(run_update(&u, none, none, 0) == VERIFY_FAIL_REASON_INCOMPLETE, "seven of eight header chunks");
    CHECK(main_slot_untouched(), "main slot untouched");
    free_update(&u);

    reset_world();
    install(0, 2, 2);
    u = make_update((ImageVersion_t){0, 2, 3}, 5, 512);
    u.tag_chunks = 3;
    CHECK(run_update(&u, none, none, 0) == VERIFY_FAIL_REASON_INCOMPLETE, "three of four tag chunks");
    free_update(&u);

    reset_world();
    install(0, 2, 2);
    u = make_update((ImageVersion_t){0, 2, 3}, 5, 512);
    u.header_chunks = 0; /* an update in the old format, with no header at all */
    CHECK(run_update(&u, none, none, 0) == VERIFY_FAIL_REASON_INCOMPLETE, "no header: the old flow is not accepted");
    free_update(&u);
}

static void test_what_the_header_says_must_be_true_of_what_arrived(void)
{
    printf("  the header against what arrived\n");
    ImageVersion_t none = {0, 0, 0}, v = {0, 2, 3};

    /* signed correctly, but for another board: the hardware id is reported as such */
    reset_world(); install(0, 2, 2);
    Update u = make_update(v, 5, 512);
    build_header(u.header, THIS_HARDWARE_ID ^ 0x100, v, u.size, MAIN_APP_ADDR, 0x20010000UL, MAIN_APP_ADDR + 0x101UL, u.declared_crc);
    sign(&u);
    CHECK(run_update(&u, none, none, 0) == VERIFY_FAIL_REASON_HARDWARE_ID, "a signed image for another board");
    CHECK(main_slot_untouched(), "main slot untouched");
    free_update(&u);

    /* signed correctly, linked for another address */
    reset_world(); install(0, 2, 2);
    u = make_update(v, 5, 512);
    build_header(u.header, THIS_HARDWARE_ID, v, u.size, MAIN_APP_ADDR + 0x800, 0x20010000UL, MAIN_APP_ADDR + 0x101UL, u.declared_crc);
    sign(&u);
    CHECK(run_update(&u, none, none, 0) == VERIFY_FAIL_REASON_HEADER, "linked for another address");
    free_update(&u);

    /* signed correctly, but the vectors the header declares are not the first words of the image */
    reset_world(); install(0, 2, 2);
    u = make_update(v, 5, 512);
    build_header(u.header, THIS_HARDWARE_ID, v, u.size, MAIN_APP_ADDR, 0x20010000UL, MAIN_APP_ADDR + 0x103UL, u.declared_crc);
    sign(&u);
    CHECK(run_update(&u, none, none, 0) == VERIFY_FAIL_REASON_HEADER, "vectors that are not the image's");
    free_update(&u);

    /* signed correctly, the header declares another size than the one that arrived */
    reset_world(); install(0, 2, 2);
    u = make_update(v, 5, 512);
    build_header(u.header, THIS_HARDWARE_ID, v, u.size + 8, MAIN_APP_ADDR, 0x20010000UL, MAIN_APP_ADDR + 0x101UL, u.declared_crc);
    sign(&u);
    CHECK(run_update(&u, none, none, 0) == VERIFY_FAIL_REASON_HEADER, "another size");
    free_update(&u);

    /* END_UPDATE announces another version than the signed header: the header is the truth */
    reset_world(); install(0, 2, 2);
    u = make_update(v, 5, 512);
    u.declared_minor = 9;
    CHECK(run_update(&u, none, none, 0) == VERIFY_FAIL_REASON_HEADER, "END_UPDATE and the header disagree");
    free_update(&u);

    /* the CRC declared in END_UPDATE is not the one of the bytes received */
    reset_world(); install(0, 2, 2);
    u = make_update(v, 5, 512);
    u.declared_crc ^= 1;
    CHECK(run_update(&u, none, none, 0) == VERIFY_FAIL_REASON_CRC32, "a wrong CRC");
    free_update(&u);

    /* a tag made with another key */
    reset_world(); install(0, 2, 2);
    u = make_update(v, 5, 512);
    u.tag[0] ^= 1;
    CHECK(run_update(&u, none, none, 0) == VERIFY_FAIL_REASON_HMAC, "a tag that is not the key's");
    free_update(&u);
}

static void test_a_flash_that_changed_after_the_transfer_breaks_the_tag(void)
{
    printf("  the image is read back from flash for the tag\n");
    ImageVersion_t none = {0, 0, 0};
    reset_world();
    install(0, 2, 2);
    Update u = make_update((ImageVersion_t){0, 2, 3}, 5, 3000);   /* more than a page: the first page is already in flash when the rest arrives */
    start(&u);
    send_body(&u);
    flash_mem[BACKUP_APP_ADDR - FLASH_BASE_ADDR + 300] ^= 0x04;   /* a bit that flipped in the backup slot */
    finish(&u);
    CHECK(last_verdict() == VERIFY_FAIL_REASON_HMAC, "the tag is computed over what is in flash, not over what was sent");
    CHECK(main_slot_untouched() && resets == 0, "and nothing is installed");
    (void)none;
    free_update(&u);
}

static void test_an_update_over_nothing_installed(void)
{
    printf("  an update with nothing installed\n");
    ImageVersion_t none = {0, 0, 0};
    reset_world();
    Update u = make_update((ImageVersion_t){0, 0, 1}, 5, 512);
    CHECK(run_update(&u, none, none, 0) == 0, "any valid signed image installs on a blank board");
    free_update(&u);
}

int main(void)
{
    printf("HYDRA-UMC host tests - G474 bootloader, the whole update\n");

    flash_mem = mmap((void *)FLASH_BASE_ADDR, FLASH_TOTAL, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (flash_mem == MAP_FAILED || (uintptr_t)flash_mem != FLASH_BASE_ADDR) {
        printf("  SKIP: cannot map the flash address range on this host\n");
        return 0;
    }

    test_the_reference_image_installs_over_the_installed_version();
    test_the_session_nonce_is_asked_for_and_changes();
    test_a_replayed_old_image_cannot_claim_a_higher_version();
    test_the_patch_counts_in_the_anti_rollback();
    test_metadata_written_before_the_patch_field_reads_as_patch_zero();
    test_a_downgrade_needs_the_token_and_only_the_right_one();
    test_the_old_constant_frame_no_longer_allows_a_downgrade();
    test_an_update_that_did_not_arrive_whole_is_refused();
    test_what_the_header_says_must_be_true_of_what_arrived();
    test_a_flash_that_changed_after_the_transfer_breaks_the_tag();
    test_an_update_over_nothing_installed();

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all G474 bootloader update host tests passed\n");
    return 0;
}

#else /* not Linux on x86-64 */

int main(void)
{
    printf("HYDRA-UMC host tests - G474 bootloader update: skipped (needs Linux on x86-64)\n");
    return 0;
}

#endif
