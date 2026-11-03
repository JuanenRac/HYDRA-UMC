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
 *  - header format 2: the Ed25519 signature over the digest of header || image, under a key of the trust table picked by the key id of the
 *    header (the development table, the next key, a revoked key, a closed table), the signed rollback counter and the metadata it leaves;
 *  - the shared cases of tests/image_format2_vectors.h, made by the host reference (tools/image_header2.py), through the same decision;
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

#include "image_format2_vectors.h"

/* The bootloader of this test carries the bench trust table (the public key of the published development seed) and the hook that lets a test
 * put another table in its place. */
#define HYDRA_TRUST_DEV 1
#define HYDRA_HOST_TEST 1

/* The development seed (sha256 of "hydra-umc public development image key v1") and its public key: published on purpose, this is the key
 * of every bench build. Only a test can sign with it: the board holds the public half. */
static const uint8_t DEV_SEED[32] = {
    0x7C, 0xCB, 0x1D, 0x59, 0x09, 0x0B, 0x41, 0x13, 0x91, 0xA7, 0xF1, 0x3D, 0x31, 0x43, 0x58, 0x04,
    0x2B, 0x0B, 0xFF, 0x4F, 0xFC, 0xB2, 0x09, 0x77, 0x22, 0xD9, 0xFB, 0xD4, 0xB9, 0x09, 0x8D, 0xC6
};
static const uint8_t DEV_PUBLIC[32] = {
    0x3F, 0x50, 0x8A, 0x29, 0xD3, 0x5D, 0x9F, 0x33, 0x85, 0xFD, 0xD5, 0x6B, 0x33, 0xD3, 0xD1, 0x21,
    0x9E, 0x35, 0xBB, 0x61, 0x80, 0x60, 0x9A, 0x83, 0x8F, 0x6D, 0x44, 0xBA, 0xCF, 0xAF, 0x80, 0xB2
};

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
#include "bootloader_trust.c"
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
/* what build_header writes in the fields of format 2 */
static uint32_t header_counter = 1;
static uint32_t installed_counter;   /* the floor of the rollback counter that install() gives the installed state */
static uint8_t  header_key_id[IMAGE_KEY_ID_SIZE] = {0x09, 0x6E, 0x13, 0x6D, 0x64, 0x08, 0xFF, 0x5B};   /* the key id of the development key */

typedef struct {
    uint8_t  header[IMAGE_HEADER_SIZE];
    uint8_t *image;
    uint32_t size;
    uint8_t  digest[IMAGE_DIGEST_SIZE];
    uint8_t  signature[IMAGE_SIGNATURE_SIZE];
    uint8_t  token[IMAGE_SIGNATURE_SIZE];
    int      token_chunks;      /* how many of the eight to send */
    int      header_chunks;     /* how many of the eight to send */
    int      signature_chunks;  /* how many of the eight to send */
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
    put_le16(header + 4, IMAGE_HEADER_FORMAT_2);
    put_le32(header + 8, hw_id);
    put_le16(header + 12, version.major);
    put_le16(header + 14, version.minor);
    put_le16(header + 16, version.patch);
    put_le32(header + 20, size);
    put_le32(header + 24, link);
    put_le32(header + 28, sp);
    put_le32(header + 32, reset);
    put_le32(header + 36, crc);
    put_le32(header + 40, header_counter);
    memcpy(header + 44, header_key_id, IMAGE_KEY_ID_SIZE);
}

/* Signs with the seed of the vectors (the private half of the key the bootloader of this test carries). Only a test can: the board cannot. */
static void sign_message(const uint8_t *message, size_t length, uint8_t signature[IMAGE_SIGNATURE_SIZE])
{
    uint8_t secret[64];
    uint8_t signed_message[IMAGE_SIGNATURE_SIZE + UPDATE_SIGNATURE_MAX_MESSAGE];
    unsigned long long signed_length = 0;
    memcpy(secret, DEV_SEED, 32);
    memcpy(secret + 32, DEV_PUBLIC, 32);
    crypto_sign(signed_message, &signed_length, message, length, secret);
    memcpy(signature, signed_message, IMAGE_SIGNATURE_SIZE);
}

/* The digest of header || image with the routine under test, and the signature over the image message made from it. */
static void sign(Update *u)
{
    memcpy(flash_mem + (SCRATCH_ADDR - FLASH_BASE_ADDR), u->image, u->size);
    sha256_prefixed_flash_region(u->header, IMAGE_HEADER_SIZE, SCRATCH_ADDR, u->size, u->digest);
    uint8_t message[IMAGE_MESSAGE_2_LEN];
    ImageHeader_SignatureMessage2(u->digest, message);
    sign_message(message, sizeof(message), u->signature);
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
    u.signature_chunks = 8;
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
    installed_counter = 0;
    header_counter = 1;
    Trust_UseTableForTest(NULL, 0);
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
    memset(m.sha256, 0x77, sizeof(m.sha256));
    m.counter = installed_counter;
    MetaRecord_t record;
    memset(&record, 0, sizeof(record));
    record.generation = 1;
    record.meta = m;
    record.crc = meta_record_crc(&record);
    memcpy(flash_mem + (METADATA_ADDR - FLASH_BASE_ADDR), &record, sizeof(record));
    memset(flash_mem + (MAIN_APP_ADDR - FLASH_BASE_ADDR), 0xA5, 4096);
}

static FirmwareMetadata_t metadata(void)
{
    FirmwareMetadata_t m;
    Metadata_Read(&m);
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
    for (int i = 0; i < u->signature_chunks; i++) HandleHmacChunk(u->signature + 8 * i);
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
        ImageHeader_DowngradeMessage(nonce, &token_installed, &token_target, u->digest, message);
        sign_message(message, sizeof(message), u->token);
        u->token_chunks = 8;
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
static uint32_t make_table(uint8_t out[100], uint8_t count, const uint8_t *key0, uint8_t flags0, const uint8_t *key1, uint8_t flags1)
{
    memset(out, 0, 100);
    memcpy(out, "TKEY", 4);
    out[4] = 1;
    out[5] = count;
    for (uint8_t i = 0; i < count; i++) {
        const uint8_t *key = (i == 0) ? key0 : key1;
        uint8_t *entry = out + 8 + i * 44;
        uint8_t digest[64];
        crypto_hash(digest, key, 32);
        memcpy(entry, digest, 8);
        memcpy(entry + 8, key, 32);
        entry[40] = (i == 0) ? flags0 : flags1;
    }
    uint32_t size = 8u + count * 44u + 4u;
    uint32_t crc = crc_of(out, size - 4u);
    put_le32(out + size - 4u, crc);
    return size;
}

static void test_the_reference_image_installs_over_the_installed_version(void)
{
    printf("  an update over the installed version, with the bytes of the host reference\n");
    reset_world();
    install(0, 2, 2);
    const Format2Case *c = &F2_CASES[0];
    uint8_t table[100];
    uint32_t size = make_table(table, 1, c->trusted[0], TRUST_FLAG_CURRENT, NULL, 0);
    Trust_UseTableForTest(table, size);
    Update u;
    memset(&u, 0, sizeof(u));
    /* the exact bytes the host reference made: header, image and signature */
    u.size = c->blob_len - IMAGE_HEADER_SIZE - IMAGE_SIGNATURE_SIZE;
    u.image = (uint8_t *)(c->blob + IMAGE_HEADER_SIZE);
    memcpy(u.header, c->blob, sizeof(u.header));
    memcpy(u.signature, c->blob + c->blob_len - IMAGE_SIGNATURE_SIZE, sizeof(u.signature));
    u.declared_crc = crc_of(u.image, u.size);
    u.declared_major = (uint16_t)(u.header[12] | (u.header[13] << 8));
    u.declared_minor = (uint16_t)(u.header[14] | (u.header[15] << 8));
    u.header_chunks = 8;
    u.signature_chunks = 8;
    ImageVersion_t none = {0, 0, 0};
    CHECK(run_update(&u, none, none, 0) == 0, "the signature the reference made is accepted through the table that holds its key");
    CHECK(resets == 1, "the board resets into the new application");
    CHECK(main_slot_holds(&u), "the image is in the main slot");
    FirmwareMetadata_t m = metadata();
    CHECK(m.state == META_STATE_APP_VALID && m.version_major == 0 && m.version_minor == 2 && m.version_patch == 3, "the metadata holds 0.2.3, the patch included");
    CHECK(m.size == u.size && m.crc32 == u.declared_crc, "size and CRC");
    CHECK(m.counter == 5, "the signed rollback counter of the header is the new floor");
    uint8_t expected[32];
    sha256_prefixed_flash_region(NULL, 0, MAIN_APP_ADDR, u.size, expected);
    CHECK(memcmp(m.sha256, expected, 32) == 0, "the boot-time check value is the SHA-256 of the image as it sits in the main slot");
    CHECK(ApplicationIsValid() == 1, "and the application validates at the next start");
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
    /* the host changes the version inside the header and in END_UPDATE: the signature no longer matches the header */
    put_le16(u.header + 12, 0);
    put_le16(u.header + 14, 9);
    put_le16(u.header + 16, 9);
    u.declared_minor = 9;
    ImageVersion_t none = {0, 0, 0};
    CHECK(run_update(&u, none, none, 0) == VERIFY_FAIL_REASON_HMAC, "the signature covers the version: changing it breaks the signature");
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
    ImageHeader_DowngradeMessage(old_nonce, &installed, &target, u.digest, message);
    uint8_t recorded[IMAGE_SIGNATURE_SIZE];
    sign_message(message, sizeof(message), recorded);
    HandleEndUpdate((uint8_t[8]){0});   /* the first session ends without installing anything */
    reset_world();
    install(0, 2, 2);
    sent_count = 0;
    start(&u);                          /* a new session: a new nonce */
    memcpy(u.token, recorded, sizeof(recorded));
    u.token_chunks = 8;
    send_body(&u);
    finish(&u);
    CHECK(last_verdict() == VERIFY_FAIL_REASON_ROLLBACK, "the token of an earlier session does not work in another");
    free_update(&u);

    /* a token is for one image: made for the digest of image u, offered with another image of the same version in a session of its own */
    reset_world();
    install(0, 2, 2);
    Update fresh = make_update(target, 11, 400);
    start(&fresh);
    uint8_t nonce[IMAGE_NONCE_SIZE];
    announced_nonce(nonce);
    u = make_update(target, 5, 400);
    ImageHeader_DowngradeMessage(nonce, &installed, &target, u.digest, message);
    sign_message(message, sizeof(message), fresh.token);
    fresh.token_chunks = 8;
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
    u.signature_chunks = 7;
    CHECK(run_update(&u, none, none, 0) == VERIFY_FAIL_REASON_INCOMPLETE, "seven of eight signature chunks");
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

    /* a signature that is not the key's */
    reset_world(); install(0, 2, 2);
    u = make_update(v, 5, 512);
    u.signature[0] ^= 1;
    CHECK(run_update(&u, none, none, 0) == VERIFY_FAIL_REASON_SIGNATURE, "a signature that is not the key's");
    free_update(&u);

    /* the four chunks an HMAC took are not a signature: the old format is not accepted */
    reset_world(); install(0, 2, 2);
    u = make_update(v, 5, 512);
    u.signature_chunks = 4;
    CHECK(run_update(&u, none, none, 0) == VERIFY_FAIL_REASON_INCOMPLETE, "an HMAC of four chunks");
    free_update(&u);

    /* a perfectly signed image that someone else's key signed */
    reset_world(); install(0, 2, 2);
    u = make_update(v, 5, 512);
    {
        /* the seed 0x90..0xAF and its public key, made by the host reference */
        static const uint8_t other_public[32] = {0xD2, 0xEB, 0x99, 0x3B, 0x63, 0x14, 0x35, 0x28, 0xE7, 0x0D, 0xCD, 0xC7, 0xEB, 0x9C, 0xA2, 0x10,
                                                 0x10, 0xDD, 0x76, 0x9B, 0xE9, 0x52, 0x50, 0xC2, 0xC5, 0xBA, 0xBF, 0xAD, 0x15, 0xD4, 0x58, 0xE2};
        uint8_t other_secret[64], signed_message[IMAGE_SIGNATURE_SIZE + IMAGE_MESSAGE_2_LEN], message[IMAGE_MESSAGE_2_LEN];
        unsigned long long length = 0;
        for (int i = 0; i < 32; i++) other_secret[i] = (uint8_t)(0x90 + i);
        memcpy(other_secret + 32, other_public, 32);
        ImageHeader_SignatureMessage2(u.digest, message);
        crypto_sign(signed_message, &length, message, sizeof(message), other_secret);
        memcpy(u.signature, signed_message, IMAGE_SIGNATURE_SIZE);
    }
    CHECK(run_update(&u, none, none, 0) == VERIFY_FAIL_REASON_SIGNATURE, "a well formed signature of somebody else's key");
    free_update(&u);
}

static void test_a_flash_that_changed_after_the_transfer_breaks_the_signature(void)
{
    printf("  the image is read back from flash for the digest\n");
    ImageVersion_t none = {0, 0, 0};
    reset_world();
    install(0, 2, 2);
    Update u = make_update((ImageVersion_t){0, 2, 3}, 5, 3000);   /* more than a page: the first page is already in flash when the rest arrives */
    start(&u);
    send_body(&u);
    flash_mem[BACKUP_APP_ADDR - FLASH_BASE_ADDR + 300] ^= 0x04;   /* a bit that flipped in the backup slot */
    finish(&u);
    CHECK(last_verdict() == VERIFY_FAIL_REASON_SIGNATURE, "the digest is computed over what is in flash, not over what was sent");
    CHECK(main_slot_untouched() && resets == 0, "and nothing is installed");
    (void)none;
    free_update(&u);
}

static void test_the_table_of_this_build_is_the_development_one(void)
{
    printf("  the trust table of this build\n");
    Trust_UseTableForTest(NULL, 0);
    CHECK(Trust_Active()->count == 1 && memcmp(Trust_Active()->keys[0], DEV_PUBLIC, 32) == 0, "the bench build trusts the development key and no other");
    CHECK(memcmp(Trust_Active()->key_id[0], header_key_id, IMAGE_KEY_ID_SIZE) == 0, "and its key id is the one the test headers name");
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

/* ---- the trust table, the counter and the metadata the update leaves ---------------------------------------------------------------- */
/* A made-up seed (0x90..0xAF) and its public key, made by the host reference: the key of "somebody else". */
static const uint8_t OTHER_PUBLIC[32] = {0xD2, 0xEB, 0x99, 0x3B, 0x63, 0x14, 0x35, 0x28, 0xE7, 0x0D, 0xCD, 0xC7, 0xEB, 0x9C, 0xA2, 0x10,
                                         0x10, 0xDD, 0x76, 0x9B, 0xE9, 0x52, 0x50, 0xC2, 0xC5, 0xBA, 0xBF, 0xAD, 0x15, 0xD4, 0x58, 0xE2};

static void other_key(uint8_t secret[64], uint8_t public_key[32])
{
    for (int i = 0; i < 32; i++) secret[i] = (uint8_t)(0x90 + i);
    memcpy(secret + 32, OTHER_PUBLIC, 32);
    memcpy(public_key, OTHER_PUBLIC, 32);
}

/* Signs the update of u with the key of `secret`, and writes its key id in the header. */
static void sign_with(Update *u, const uint8_t secret[64], const uint8_t public_key[32])
{
    uint8_t digest64[64];
    crypto_hash(digest64, public_key, 32);
    memcpy(u->header + 44, digest64, IMAGE_KEY_ID_SIZE);
    memcpy(flash_mem + (SCRATCH_ADDR - FLASH_BASE_ADDR), u->image, u->size);
    sha256_prefixed_flash_region(u->header, IMAGE_HEADER_SIZE, SCRATCH_ADDR, u->size, u->digest);
    uint8_t message[IMAGE_MESSAGE_2_LEN], signed_message[IMAGE_SIGNATURE_SIZE + IMAGE_MESSAGE_2_LEN];
    unsigned long long length = 0;
    ImageHeader_SignatureMessage2(u->digest, message);
    crypto_sign(signed_message, &length, message, sizeof(message), secret);
    memcpy(u->signature, signed_message, IMAGE_SIGNATURE_SIZE);
}

static void test_the_key_of_the_header_picks_the_key_of_the_table(void)
{
    printf("  the key id of the header and the trust table\n");
    ImageVersion_t none = {0, 0, 0}, v = {0, 2, 3};
    uint8_t secret[64], other_public[32], table[100];
    other_key(secret, other_public);

    reset_world(); install(0, 2, 2);
    Update u = make_update(v, 5, 512);
    sign_with(&u, secret, other_public);
    CHECK(run_update(&u, none, none, 0) == VERIFY_FAIL_REASON_SIGNATURE, "a key the table does not hold");
    CHECK(main_slot_untouched() && resets == 0, "nothing was installed");

    reset_world(); install(0, 2, 2);
    uint32_t size = make_table(table, 2, DEV_PUBLIC, TRUST_FLAG_CURRENT, other_public, TRUST_FLAG_NEXT);
    Trust_UseTableForTest(table, size);
    CHECK(run_update(&u, none, none, 0) == 0, "the same update installs when the table lists that key as the next one");

    reset_world(); install(0, 2, 2);
    size = make_table(table, 2, DEV_PUBLIC, TRUST_FLAG_CURRENT, other_public, TRUST_FLAG_REVOKED);
    Trust_UseTableForTest(table, size);
    CHECK(run_update(&u, none, none, 0) == VERIFY_FAIL_REASON_SIGNATURE && main_slot_untouched(), "a revoked key is refused though its signature is valid");

    reset_world(); install(0, 2, 2);
    size = make_table(table, 0, NULL, 0, NULL, 0);
    Trust_UseTableForTest(table, size);
    free_update(&u);
    u = make_update(v, 5, 512);
    CHECK(run_update(&u, none, none, 0) == VERIFY_FAIL_REASON_SIGNATURE && main_slot_untouched(), "a closed table installs nothing, not even a signature of the development key");

    reset_world(); install(0, 2, 2);
    size = make_table(table, 1, DEV_PUBLIC, TRUST_FLAG_CURRENT, NULL, 0);
    table[20] ^= 0x04;
    Trust_UseTableForTest(table, size);
    CHECK(run_update(&u, none, none, 0) == VERIFY_FAIL_REASON_SIGNATURE && main_slot_untouched(), "a table with a bad CRC trusts nobody");
    free_update(&u);
}

static void test_the_signed_counter_is_a_floor_nothing_overrides(void)
{
    printf("  the rollback counter\n");
    ImageVersion_t none = {0, 0, 0}, v = {0, 2, 3};
    reset_world(); install(0, 2, 2);
    installed_counter = 5;
    install(0, 2, 2);
    header_counter = 3;
    Update u = make_update(v, 5, 512);
    CHECK(run_update(&u, none, none, 0) == VERIFY_FAIL_REASON_ROLLBACK, "a counter below the floor is refused though the version is newer");
    CHECK(main_slot_untouched(), "main slot untouched");
    free_update(&u);

    reset_world(); installed_counter = 5; install(0, 2, 2);
    header_counter = 5;
    u = make_update(v, 5, 512);
    CHECK(run_update(&u, none, none, 0) == 0 && metadata().counter == 5, "a counter equal to the floor installs");
    free_update(&u);

    reset_world(); installed_counter = 5; install(0, 2, 2);
    header_counter = 9;
    u = make_update(v, 5, 512);
    CHECK(run_update(&u, none, none, 0) == 0 && metadata().counter == 9, "a higher counter installs and becomes the floor");
    free_update(&u);

    /* the token of a downgrade lowers the version rule and never the counter */
    ImageVersion_t installed = {0, 2, 2}, target = {0, 1, 8};
    reset_world(); installed_counter = 5; install(0, 2, 2);
    header_counter = 3;
    u = make_update(target, 5, 400);
    CHECK(run_update(&u, installed, target, 1) == VERIFY_FAIL_REASON_ROLLBACK && main_slot_untouched(), "a valid downgrade token does not override the counter");
    free_update(&u);

    /* a board with no state keeps the floor compiled into the bootloader */
    reset_world();
    CHECK(Trust_MinCounter() == (uint32_t)HYDRA_TRUST_MIN_COUNTER, "the compiled floor is the one of the public file");
}

static int expected_verdict(int status)
{
    switch (status) {
        case F2_STATUS_OK: return UPDATE_VERDICT_OK;
        case F2_STATUS_TOO_SHORT: return UPDATE_VERDICT_INCOMPLETE;
        case F2_STATUS_UNKNOWN_FORMAT:
        case F2_STATUS_BAD_HEADER:
        case F2_STATUS_BAD_CRC:
        case F2_STATUS_LINK_MISMATCH:
        case F2_STATUS_BAD_VECTORS: return UPDATE_VERDICT_HEADER;
        case F2_STATUS_SIZE_MISMATCH: return -2;   /* a framing error: the digest of the longer image fails first, or the header's size does */
        case F2_STATUS_UNKNOWN_KEY: return UPDATE_VERDICT_UNKNOWN_KEY;
        case F2_STATUS_BAD_SIGNATURE: return UPDATE_VERDICT_SIGNATURE;
        case F2_STATUS_HARDWARE_MISMATCH: return UPDATE_VERDICT_HARDWARE_ID;
        case F2_STATUS_ROLLBACK_COUNTER: return UPDATE_VERDICT_COUNTER;
        case F2_STATUS_ROLLBACK_VERSION: return UPDATE_VERDICT_ROLLBACK;
        default: return -3;
    }
}

static int verify_under_key(const uint8_t *msg, uint32_t len, const uint8_t signature[IMAGE_SIGNATURE_SIZE], const uint8_t public_key[32], void *context)
{
    (void)context;
    return UpdateSignature_Verify(msg, len, signature, public_key);
}

/* Every case of the host reference through the decision of the bootloader (update_verify.h), with the real SHA-256 of the bootloader and
 * the real Ed25519 check. */
static void test_the_shared_cases_get_the_verdict_of_the_host_reference(void)
{
    printf("  the shared cases of tests/image_format2_vectors.h\n");
    int wrong = 0;
    for (unsigned n = 0; n < F2_CASE_COUNT; n++) {
        const Format2Case *c = &F2_CASES[n];
        reset_world();
        UpdateVerify2Input_t in;
        memset(&in, 0, sizeof(in));
        uint8_t header[IMAGE_HEADER_SIZE] = {0}, signature[IMAGE_SIGNATURE_SIZE] = {0}, digest[IMAGE_DIGEST_SIZE] = {0};
        uint8_t key_ids[2][IMAGE_KEY_ID_SIZE];
        const uint8_t *image = c->blob + IMAGE_HEADER_SIZE;
        uint32_t size = 0;
        if (c->blob_len >= IMAGE_HEADER_SIZE + IMAGE_SIGNATURE_SIZE) {
            size = c->blob_len - IMAGE_HEADER_SIZE - IMAGE_SIGNATURE_SIZE;
            memcpy(header, c->blob, IMAGE_HEADER_SIZE);
            memcpy(signature, c->blob + c->blob_len - IMAGE_SIGNATURE_SIZE, IMAGE_SIGNATURE_SIZE);
            memcpy(flash_mem + (SCRATCH_ADDR - FLASH_BASE_ADDR), image, size);
            sha256_prefixed_flash_region(header, IMAGE_HEADER_SIZE, SCRATCH_ADDR, size, digest);
            in.header_chunks = 8;
            in.signature_chunks = 8;
        }
        ImageHeader2_t parsed;
        uint16_t major = 0, minor = 0;
        if (ImageHeader_Parse2(header, &parsed) == IMAGE_HEADER_OK) {
            major = parsed.base.version.major;
            minor = parsed.base.version.minor;
        }
        for (unsigned k = 0; k < c->trusted_count; k++) {
            uint8_t digest64[64];
            crypto_hash(digest64, c->trusted[k], 32);
            memcpy(key_ids[k], digest64, IMAGE_KEY_ID_SIZE);
        }
        in.bytes_received = size;
        in.total_size = size;
        in.declared_crc = crc_of(image, size);
        in.actual_crc = in.declared_crc;
        in.declared_major = major;
        in.declared_minor = minor;
        in.header = header;
        in.signature = signature;
        in.actual_digest = digest;
        in.token = c->token;
        in.token_chunks = c->token_len == 64 ? 8 : 0;
        uint8_t nonce[IMAGE_NONCE_SIZE] = {0};
        if (c->nonce_len == IMAGE_NONCE_SIZE) memcpy(nonce, c->nonce, sizeof(nonce));
        in.nonce = nonce;
        in.expect.hw_id = c->hw_id;
        in.expect.link_address = c->link_address;
        in.expect.first_word = size >= 8 ? (uint32_t)(image[0] | (image[1] << 8) | (image[2] << 16) | ((uint32_t)image[3] << 24)) : 0;
        in.expect.second_word = size >= 8 ? (uint32_t)(image[4] | (image[5] << 8) | (image[6] << 16) | ((uint32_t)image[7] << 24)) : 0;
        in.expect.ram_lo = SRAM_BASE + 0x100UL;
        in.expect.ram_hi = SRAM_BASE + 0x18000UL;
        in.have_installed = 1;
        in.installed.major = c->installed[0];
        in.installed.minor = c->installed[1];
        in.installed.patch = c->installed[2];
        in.counter_floor = c->floor;
        in.key_count = c->trusted_count;
        in.key_ids = (const uint8_t (*)[IMAGE_KEY_ID_SIZE])key_ids;
        in.keys = (const uint8_t (*)[32])c->trusted;
        in.verify = verify_under_key;
        UpdateVerdict_t got = UpdateVerify_Decide2(&in, NULL);
        int want = expected_verdict(c->expected);
        int ok = want == -2 ? (got == UPDATE_VERDICT_HEADER || got == UPDATE_VERDICT_SIGNATURE) : (int)got == want;
        if (!ok) {
            printf("    case %u (%s): verdict %d, expected %d\n", n, c->name, (int)got, want);
            wrong++;
        }
    }
    CHECK(wrong == 0, "every case of the host reference gets the verdict it gets there");
}

static void test_the_metadata_after_an_update_and_a_cut_during_it(void)
{
    printf("  the metadata log across an update\n");
    ImageVersion_t none = {0, 0, 0};
    reset_world(); install(0, 2, 2);
    Update u = make_update((ImageVersion_t){0, 2, 3}, 5, 512);
    header_counter = 1;
    CHECK(run_update(&u, none, none, 0) == 0, "an update installs");
    const MetaRecord_t *first = (const MetaRecord_t *)(flash_mem + (METADATA_ADDR - FLASH_BASE_ADDR));
    CHECK(first[0].generation == 1 && first[1].generation == 2 && first[2].generation == 3, "the old state, the pending copy and the done state are three records");
    CHECK(first[1].meta.state == META_STATE_COPY_PENDING && first[2].meta.state == META_STATE_APP_VALID, "in that order, nothing erased between them");
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
    test_the_table_of_this_build_is_the_development_one();
    test_the_key_of_the_header_picks_the_key_of_the_table();
    test_the_signed_counter_is_a_floor_nothing_overrides();
    test_the_shared_cases_get_the_verdict_of_the_host_reference();
    test_the_metadata_after_an_update_and_a_cut_during_it();
    test_a_flash_that_changed_after_the_transfer_breaks_the_signature();
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
