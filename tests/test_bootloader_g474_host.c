/* =============================================================================
 * HYDRA-UMC Firmware - Host logic tests for the G474 bootloader's SHA-256,
 * Ed25519 check, CRC32 and flash erase/program/copy/metadata-log routines
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * The two bootloader sources are compiled as they are (included below) against
 * tests/stubs/stm32g4xx_hal.h; this file plays the flash controller (a 512 KB
 * array mapped at the real flash address, erase by bank and page, programming
 * only over erased double-words) and counts the watchdog refreshes.
 *
 *  - SHA-256 against FIPS 180 digests and every padding boundary of the block,
 *    and the constant-time comparison.
 *  - SHA-256 of header || image against the host reference, and the Ed25519 check
 *    (update_signature.c, linked with the vendored TweetNaCl) against the RFC 8032
 *    vectors and the signatures tools/image_header.py made.
 *  - CRC32 against the zlib value.
 *  - Flash_ErasePages: bank/page of each address, the dual-bank refusal, a
 *    failure in the middle, one watchdog refresh per page.
 *  - Flash_WriteVerified: padding of a partial double-word, a refused program, a
 *    stuck cell found by the read-back, the lock after every path.
 *  - Flash_CopyRegion on top of them, and the metadata log: records appended
 *    without an erase, two pages used in turn, every power-cut point of a record
 *    and of the change of page.
 *
 * Needs Linux on x86-64 (a fixed mapping at the flash address, and addresses
 * that fit in 32 bits); anywhere else it reports that and passes.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "image_header_vectors.h"
#include "update_signature.h"
#include "image_header.h"

#if defined(__linux__) && defined(__x86_64__)
#include <sys/mman.h>

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

/* ---- what the bootloader sources expect the board to define ------------- */
FDCAN_HandleTypeDef hfdcan1;
IWDG_HandleTypeDef hiwdg;
FLASH_TypeDef host_flash_registers;

static int heartbeat_calls;
static uint8_t heartbeat_status[64];
static uint8_t heartbeat_percent[64];

void CAN_SendHeartbeat(uint8_t status, uint8_t progress_percent)
{
    if (heartbeat_calls < 64) {
        heartbeat_status[heartbeat_calls] = status;
        heartbeat_percent[heartbeat_calls] = progress_percent;
    }
    heartbeat_calls++;
}

/* ---- the flash controller ------------------------------------------------ */
#define FLASH_BASE_ADDR  0x08000000UL
#define FLASH_TOTAL      0x00080000UL
#define BANK_BYTES       0x00040000UL

static uint8_t *flash_mem;
static int iwdg_refreshes;
static int unlocks, locks;
static int is_unlocked;
static int erase_calls;
static uint32_t erase_bank[512];
static uint32_t erase_page[512];
static int fail_erase_on_call;      /* 1-based call number that fails; 0 = never */
static int fail_program_on_call;    /* 1-based call number that fails; 0 = never */
static int program_calls;
static int stuck_cell_at_call;      /* after this programming call the stored value is corrupted */
static int program_error_not_erased;

HAL_StatusTypeDef HAL_FLASH_Unlock(void) { unlocks++; is_unlocked = 1; return HAL_OK; }
HAL_StatusTypeDef HAL_FLASH_Lock(void) { locks++; is_unlocked = 0; return HAL_OK; }
HAL_StatusTypeDef HAL_IWDG_Refresh(IWDG_HandleTypeDef *h) { (void)h; iwdg_refreshes++; return HAL_OK; }

HAL_StatusTypeDef HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef *init, uint32_t *page_error)
{
    erase_calls++;
    if (erase_calls < 512) {
        erase_bank[erase_calls - 1] = init->Banks;
        erase_page[erase_calls - 1] = init->Page;
    }
    *page_error = 0xFFFFFFFFUL;
    if (!is_unlocked || init->TypeErase != FLASH_TYPEERASE_PAGES || init->NbPages != 1U) {
        *page_error = init->Page;
        return HAL_ERROR;
    }
    if (fail_erase_on_call && erase_calls == fail_erase_on_call) {
        *page_error = init->Page;
        return HAL_ERROR;
    }
    if ((init->Banks != FLASH_BANK_1 && init->Banks != FLASH_BANK_2) || init->Page > 127U) {
        *page_error = init->Page;
        return HAL_ERROR;
    }
    uint32_t offset = (init->Banks == FLASH_BANK_2 ? BANK_BYTES : 0UL) + init->Page * FLASH_PAGE_SIZE;
    memset(flash_mem + offset, 0xFF, FLASH_PAGE_SIZE);
    return HAL_OK;
}

HAL_StatusTypeDef HAL_FLASH_Program(uint32_t type, uint32_t address, uint64_t data)
{
    program_calls++;
    if (!is_unlocked || type != FLASH_TYPEPROGRAM_DOUBLEWORD) return HAL_ERROR;
    if (fail_program_on_call && program_calls == fail_program_on_call) return HAL_ERROR;
    if ((address & 7UL) != 0 || address < FLASH_BASE_ADDR || address + 8 > FLASH_BASE_ADDR + FLASH_TOTAL) return HAL_ERROR;
    uint8_t *cell = flash_mem + (address - FLASH_BASE_ADDR);
    for (int i = 0; i < 8; i++) {
        if (cell[i] != 0xFF) { program_error_not_erased++; return HAL_ERROR; } /* real flash refuses a double-word that is not erased */
    }
    memcpy(cell, &data, 8);
    if (stuck_cell_at_call && program_calls == stuck_cell_at_call) cell[3] ^= 0x10;
    return HAL_OK;
}

/* The sources under test. */
#include "bootloader_crypto.c"
#include "bootloader_flash.c"

/* ---- helpers ------------------------------------------------------------- */
static void reset_flash_model(void)
{
    memset(flash_mem, 0xFF, FLASH_TOTAL);
    host_flash_registers.OPTR = FLASH_OPTR_DBANK;
    iwdg_refreshes = unlocks = locks = is_unlocked = 0;
    erase_calls = program_calls = 0;
    fail_erase_on_call = fail_program_on_call = stuck_cell_at_call = 0;
    program_error_not_erased = 0;
    heartbeat_calls = 0;
}

static uint8_t *low_buffer(size_t n)
{
    void *p = mmap(NULL, n ? n : 1, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (p == MAP_FAILED) { printf("  cannot map a low buffer\n"); exit(1); }
    return (uint8_t *)p;
}

static void sha_of(const uint8_t *data, uint32_t len, uint8_t out[32])
{
    uint8_t *low = low_buffer(len);
    if (len) memcpy(low, data, len);
    sha256_prefixed_flash_region(NULL, 0, (uint32_t)(uintptr_t)low, len, out);
    munmap(low, len ? len : 1);
}

static int hex_equals(const uint8_t *bytes, const char *hex)
{
    char text[65];
    for (int i = 0; i < 32; i++) sprintf(&text[i * 2], "%02x", bytes[i]);
    return strcmp(text, hex) == 0;
}

/* ---- SHA-256 ------------------------------------------------------------- */
static void test_sha256_padding_boundaries(void)
{
    printf("  SHA-256, every padding boundary of the 64-byte block\n");
    static const struct { uint32_t len; const char *expected; } cases[] = {
        {0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {1, "084fed08b978af4d7d196a7446a86b58009e636b611db16211b65a9aadff29c5"},
        {55, "e7313d333c272e639f790978283f9eb392e843d0f29b7016828bb1daa4aac70b"},
        {56, "4324d65f3c103567f5589c710bc08f8523f929a9272e3af36fc968e52abc6c27"},
        {57, "35df609437dcfea3279283ab79fd554e2bf78f8f7ae2de532d8ee300b09e8f73"},
        {63, "81c80242132f230c3bd41b3e63bbcff16107339549214a99614ff26664625055"},
        {64, "39e3d7b6b5d075d37d053ad89b24b41bef4f3c29760c84447cab3f3be1882241"},
        {65, "aacca6ff74fdbb296d165a45cecfa04e5127bc008770fbbdd48006f2d2fae95e"},
        {119, "9ce7368e4daf32341631b492e80359dc9f594b48453cd0dd5bf0b19279cc177e"},
        {120, "7836b787757e95e58b3ca5aec90b1b004e8deba1e50e9675af9cabf1a13a04b5"},
        {121, "1189a98a00c71bc1848ea8bdc9700b442bee0be7c3f45172303f1ab0b6f1617e"},
        {127, "a8d23e75d936f303d248888d9b165ee543f4cbafcad3c9dd2a79bd84faa11d07"},
        {128, "d2742f1f4ac6bb7ca2b239ee18402ba8b3f9f8e652d2a72973c2b9ba11c08cf6"},
        {129, "307f8fc2c1622b92762e818d39a185d4d667ad49a4b07ceae1f4afa008a93ec4"},
        {1000, "1e9bc38cbf860b9ec31918b065f9b52476c549a782e0e7990bed8ce3868d2371"}
    };
    uint8_t data[1000];
    uint8_t out[32];
    for (int i = 0; i < 1000; i++) data[i] = (uint8_t)((i * 7 + 3) & 0xFF);
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        char what[48];
        snprintf(what, sizeof(what), "%u bytes", cases[c].len);
        sha_of(data, cases[c].len, out);
        CHECK(hex_equals(out, cases[c].expected), what);
    }
}

/* The digest of a signed image is the SHA-256 of its 64-byte header (a RAM buffer) followed by the image (flash) as ONE message. The bytes
 * come from tools/image_header.py through tests/image_header_vectors.h, so this checks the C routine against the reference, not itself. */
static void test_the_digest_of_a_header_and_an_image_matches_the_reference(void)
{
    printf("  SHA-256 of header || image, against the host reference\n");
    reset_flash_model();
    memcpy(flash_mem + (BACKUP_APP_ADDR - FLASH_BASE_ADDR), VEC_IMAGE, VEC_IMAGE_SIZE);
    uint8_t out[32];
    sha256_prefixed_flash_region(VEC_HEADER, sizeof(VEC_HEADER), BACKUP_APP_ADDR, VEC_IMAGE_SIZE, out);
    CHECK(digest_constant_time_compare(out, VEC_DIGEST, 32) == 1, "the digest the reference made for the image matches");

    /* one changed bit anywhere in the header or the image is another digest */
    uint8_t header[sizeof(VEC_HEADER)];
    for (size_t i = 0; i < sizeof(header); i += 9) {
        memcpy(header, VEC_HEADER, sizeof(header));
        header[i] ^= 0x01;
        sha256_prefixed_flash_region(header, sizeof(header), BACKUP_APP_ADDR, VEC_IMAGE_SIZE, out);
        CHECK(digest_constant_time_compare(out, VEC_DIGEST, 32) == 0, "a changed header byte changes the digest");
    }
    for (uint32_t i = 0; i < VEC_IMAGE_SIZE; i += 61) {
        flash_mem[BACKUP_APP_ADDR - FLASH_BASE_ADDR + i] ^= 0x01;
        sha256_prefixed_flash_region(VEC_HEADER, sizeof(VEC_HEADER), BACKUP_APP_ADDR, VEC_IMAGE_SIZE, out);
        CHECK(digest_constant_time_compare(out, VEC_DIGEST, 32) == 0, "a changed image byte changes the digest");
        flash_mem[BACKUP_APP_ADDR - FLASH_BASE_ADDR + i] ^= 0x01;
    }
    /* the image alone, as the old flow covered, is not the digest of the signed image */
    sha256_prefixed_flash_region(NULL, 0, BACKUP_APP_ADDR, VEC_IMAGE_SIZE, out);
    CHECK(digest_constant_time_compare(out, VEC_DIGEST, 32) == 0, "the SHA-256 of the image alone is not the digest of the signed image");
}

static void test_sha256_buffer_and_the_split_between_prefix_and_region(void)
{
    printf("  SHA-256: FIPS 180 vectors and the split between the prefix and the region\n");
    reset_flash_model();
    uint8_t out[32];
    sha256_buffer((const uint8_t *)"abc", 3, out);
    CHECK(hex_equals(out, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "SHA-256 of abc");
    sha256_buffer(NULL, 0, out);
    CHECK(hex_equals(out, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"), "SHA-256 of nothing");
    const char *two_blocks = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256_buffer((const uint8_t *)two_blocks, (uint32_t)strlen(two_blocks), out);
    CHECK(hex_equals(out, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"), "SHA-256 of the two-block message");

    uint8_t data[200], whole[32], parts[32];
    for (int i = 0; i < 200; i++) data[i] = (uint8_t)(i * 5 + 7);
    sha256_buffer(data, 200, whole);
    static const uint32_t splits[] = {0, 1, 55, 56, 63, 64, 65, 127, 128, 199, 200};
    for (size_t k = 0; k < sizeof(splits) / sizeof(splits[0]); k++) {
        uint32_t split = splits[k];
        uint8_t *low = low_buffer(200 - split + 1);
        memcpy(low, data + split, 200 - split);
        sha256_prefixed_flash_region(data, split, (uint32_t)(uintptr_t)low, 200 - split, parts);
        char what[48];
        snprintf(what, sizeof(what), "prefix of %u bytes", split);
        CHECK(digest_constant_time_compare(whole, parts, 32) == 1, what);
        munmap(low, 200 - split + 1);
    }
}

static void sig_from_hex(const char *hex, uint8_t *out, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        unsigned value = 0;
        sscanf(hex + 2 * i, "%2x", &value);
        out[i] = (uint8_t)value;
    }
}

/* The Ed25519 check of the bootloader against the vectors of RFC 8032 section 7.1 (a key, a message and the signature made by someone else's
 * implementation), then against the signatures tools/image_header.py made for the image and the downgrade. */
static void test_the_signature_check_matches_rfc_8032_and_the_reference(void)
{
    printf("  Ed25519 check, RFC 8032 vectors and the host reference\n");
    uint8_t public_key[32], signature[64];
    sig_from_hex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", public_key, 32);
    sig_from_hex("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b", signature, 64);
    CHECK(UpdateSignature_Verify((const uint8_t *)"", 0, signature, public_key) == 1, "RFC 8032 test 1 (empty message)");
    signature[10] ^= 1;
    CHECK(UpdateSignature_Verify((const uint8_t *)"", 0, signature, public_key) == 0, "RFC 8032 test 1 with a bit of R changed");
    sig_from_hex("3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", public_key, 32);
    sig_from_hex("92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00", signature, 64);
    const uint8_t one_byte = 0x72;
    CHECK(UpdateSignature_Verify(&one_byte, 1, signature, public_key) == 1, "RFC 8032 test 2 (one byte)");
    const uint8_t other_byte = 0x73;
    CHECK(UpdateSignature_Verify(&other_byte, 1, signature, public_key) == 0, "RFC 8032 test 2 with another message");

    CHECK(UpdateSignature_Verify(VEC_IMAGE_MESSAGE, sizeof(VEC_IMAGE_MESSAGE), VEC_SIGNATURE, VEC_PUBLIC_KEY) == 1, "the signature of image A");
    CHECK(UpdateSignature_Verify(VEC_B_IMAGE_MESSAGE, sizeof(VEC_B_IMAGE_MESSAGE), VEC_B_SIGNATURE, VEC_PUBLIC_KEY) == 1, "the signature of image B");
    CHECK(UpdateSignature_Verify(VEC_DOWNGRADE_MESSAGE, sizeof(VEC_DOWNGRADE_MESSAGE), VEC_DOWNGRADE_TOKEN, VEC_PUBLIC_KEY) == 1, "the downgrade token the reference made");
    /* a signature made for one thing is not valid for the other, nor for another image */
    CHECK(UpdateSignature_Verify(VEC_IMAGE_MESSAGE, sizeof(VEC_IMAGE_MESSAGE), VEC_B_SIGNATURE, VEC_PUBLIC_KEY) == 0, "the signature of B does not open A");
    CHECK(UpdateSignature_Verify(VEC_DOWNGRADE_MESSAGE, sizeof(VEC_DOWNGRADE_MESSAGE), VEC_SIGNATURE, VEC_PUBLIC_KEY) == 0, "an image signature is not a downgrade token");

    uint8_t message[sizeof(VEC_DOWNGRADE_MESSAGE)];
    memcpy(message, VEC_DOWNGRADE_MESSAGE, sizeof(message));
    message[sizeof(message) - 1] ^= 1; /* the digest of another image */
    CHECK(UpdateSignature_Verify(message, sizeof(message), VEC_DOWNGRADE_TOKEN, VEC_PUBLIC_KEY) == 0, "a token for another image is refused");
    memcpy(message, VEC_DOWNGRADE_MESSAGE, sizeof(message));
    message[DOWNGRADE_TAG_LEN] ^= 1; /* another session */
    CHECK(UpdateSignature_Verify(message, sizeof(message), VEC_DOWNGRADE_TOKEN, VEC_PUBLIC_KEY) == 0, "a token for another session is refused");

    /* every byte of the signature matters, and so does the key */
    for (size_t i = 0; i < 64; i += 7) {
        memcpy(signature, VEC_SIGNATURE, 64);
        signature[i] ^= 0x01;
        CHECK(UpdateSignature_Verify(VEC_IMAGE_MESSAGE, sizeof(VEC_IMAGE_MESSAGE), signature, VEC_PUBLIC_KEY) == 0, "a changed signature byte is refused");
    }
    memcpy(public_key, VEC_PUBLIC_KEY, 32);
    public_key[5] ^= 1;
    CHECK(UpdateSignature_Verify(VEC_IMAGE_MESSAGE, sizeof(VEC_IMAGE_MESSAGE), VEC_SIGNATURE, public_key) == 0, "another key is refused");
    /* S not reduced modulo the order of the base point is refused even when the equation still holds (S + L) */
    memcpy(signature, VEC_SIGNATURE, 64);
    {
        static const uint8_t order[32] = {0xED, 0xD3, 0xF5, 0x5C, 0x1A, 0x63, 0x12, 0x58, 0xD6, 0x9C, 0xF7, 0xA2, 0xDE, 0xF9, 0xDE, 0x14,
                                          0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10};
        unsigned carry = 0;
        for (int i = 0; i < 32; i++) {
            unsigned sum = (unsigned)signature[32 + i] + order[i] + carry;
            signature[32 + i] = (uint8_t)sum;
            carry = sum >> 8;
        }
        CHECK(carry == 0, "S + L fits in 32 bytes for the vector");
    }
    CHECK(UpdateSignature_Verify(VEC_IMAGE_MESSAGE, sizeof(VEC_IMAGE_MESSAGE), signature, VEC_PUBLIC_KEY) == 0, "S + L is refused");
    /* a message longer than the check takes, and null pointers, are refused instead of read */
    uint8_t long_message[UPDATE_SIGNATURE_MAX_MESSAGE + 1] = {0};
    CHECK(UpdateSignature_Verify(long_message, sizeof(long_message), VEC_SIGNATURE, VEC_PUBLIC_KEY) == 0, "a message that is too long");
    CHECK(UpdateSignature_Verify(NULL, 0, VEC_SIGNATURE, VEC_PUBLIC_KEY) == 0, "no message");
}

static void test_the_signature_check_keeps_the_watchdog_fed(void)
{
    printf("  Ed25519 check keeps the watchdog fed\n");
    iwdg_refreshes = 0;
    CHECK(UpdateSignature_Verify(VEC_IMAGE_MESSAGE, sizeof(VEC_IMAGE_MESSAGE), VEC_SIGNATURE, VEC_PUBLIC_KEY) == 1, "the signature verifies");
    CHECK(iwdg_refreshes >= 256, "at least one refresh per bit of a scalar");
}

static void test_sha256_feeds_the_watchdog_on_a_large_region(void)
{
    printf("  SHA-256 keeps the watchdog fed on a large region\n");
    reset_flash_model();
    uint8_t out[32];
    uint32_t len = 100000;
    uint8_t *data = calloc(1, len);
    sha_of(data, len, out);
    free(data);
    CHECK(iwdg_refreshes >= (int)(len / 4096U), "at least one refresh per 4096 bytes");
}

static void test_constant_time_compare(void)
{
    printf("  constant-time comparison\n");
    uint8_t a[32], b[32];
    memset(a, 0x5a, 32);
    memcpy(b, a, 32);
    CHECK(digest_constant_time_compare(a, b, 32) == 1, "equal buffers");
    for (int i = 0; i < 32; i++) {
        b[i] ^= 0x01;
        CHECK(digest_constant_time_compare(a, b, 32) == 0, "a single changed bit anywhere is a mismatch");
        b[i] ^= 0x01;
    }
    CHECK(digest_constant_time_compare(a, b, 0) == 1, "nothing to compare is equal");
}

/* ---- CRC32 --------------------------------------------------------------- */
static void test_crc32(void)
{
    printf("  CRC32\n");
    uint32_t crc = CRC32_Finalize(CRC32_Update(0xFFFFFFFFUL, (const uint8_t *)"123456789", 9));
    CHECK(crc == 0xCBF43926UL, "the standard check value");
    CHECK(CRC32_Finalize(0xFFFFFFFFUL) == 0UL, "empty input");
    uint8_t zeros[1024] = {0};
    CHECK(CRC32_Finalize(CRC32_Update(0xFFFFFFFFUL, zeros, 1024)) == 0xEFB5AF2EUL, "1024 zero bytes");
    CHECK(CRC32_Finalize(CRC32_Update(0xFFFFFFFFUL, (const uint8_t *)"hello world", 11)) == 0x0D4A1185UL, "hello world");
    uint32_t split = CRC32_Update(0xFFFFFFFFUL, (const uint8_t *)"1234", 4);
    split = CRC32_Update(split, (const uint8_t *)"56789", 5);
    CHECK(CRC32_Finalize(split) == 0xCBF43926UL, "feeding it in pieces gives the same value");
}

/* ---- erase by bank and page ----------------------------------------------- */
static void test_erase_maps_each_address_to_bank_and_page(void)
{
    printf("  Flash_ErasePages: bank and page of each address\n");
    reset_flash_model();
    CHECK(Flash_ErasePages(MAIN_APP_ADDR, 1) == 1, "main application page");
    CHECK(erase_calls == 1 && erase_bank[0] == FLASH_BANK_1 && erase_page[0] == 18, "0x08009000 is bank 1, page 18");
    CHECK(Flash_ErasePages(BACKUP_APP_ADDR, 1) == 1, "backup slot page");
    CHECK(erase_bank[1] == FLASH_BANK_2 && erase_page[1] == 8, "0x08044000 is bank 2, page 8 (not page 136)");
    CHECK(Flash_ErasePages(METADATA_ADDR, 1) == 1, "metadata page");
    CHECK(erase_bank[2] == FLASH_BANK_1 && erase_page[2] == 16, "0x08008000 is bank 1, page 16");
    CHECK(Flash_ErasePages(0x0807F800UL, 1) == 1, "last page of the flash");
    CHECK(erase_bank[3] == FLASH_BANK_2 && erase_page[3] == 127, "0x0807F800 is bank 2, page 127");
}

static void test_erase_crosses_the_bank_boundary_and_feeds_the_watchdog_per_page(void)
{
    printf("  Flash_ErasePages: across the bank boundary\n");
    reset_flash_model();
    CHECK(Flash_ErasePages(0x0803F800UL, 3) == 1, "three pages from the last page of bank 1");
    CHECK(erase_calls == 3, "one erase call per page");
    CHECK(erase_bank[0] == FLASH_BANK_1 && erase_page[0] == 127, "first page: bank 1, page 127");
    CHECK(erase_bank[1] == FLASH_BANK_2 && erase_page[1] == 0, "second page: bank 2, page 0");
    CHECK(erase_bank[2] == FLASH_BANK_2 && erase_page[2] == 1, "third page: bank 2, page 1");
    CHECK(iwdg_refreshes == 3, "one watchdog refresh per page");
    CHECK(unlocks == locks && !is_unlocked, "the flash is locked again");
}

static void test_erase_only_touches_the_requested_page(void)
{
    printf("  Flash_ErasePages: neighbours are left alone\n");
    reset_flash_model();
    memset(flash_mem, 0x00, FLASH_TOTAL);
    CHECK(Flash_ErasePages(BACKUP_APP_ADDR, 1) == 1, "erase");
    uint32_t off = BACKUP_APP_ADDR - FLASH_BASE_ADDR;
    CHECK(flash_mem[off] == 0xFF && flash_mem[off + FLASH_PAGE_SIZE - 1] == 0xFF, "the page is erased");
    CHECK(flash_mem[off - 1] == 0x00 && flash_mem[off + FLASH_PAGE_SIZE] == 0x00, "the pages before and after are untouched");
    CHECK(flash_mem[METADATA_ADDR - FLASH_BASE_ADDR] == 0x00, "the metadata page is untouched");
}

static void test_erase_refuses_what_it_cannot_do_safely(void)
{
    printf("  Flash_ErasePages: refusals\n");
    reset_flash_model();
    host_flash_registers.OPTR = 0;
    CHECK(Flash_ErasePages(MAIN_APP_ADDR, 1) == 0, "single-bank mode: the page numbering would be wrong");
    CHECK(erase_calls == 0, "nothing is erased in single-bank mode");

    reset_flash_model();
    CHECK(Flash_ErasePages(0x08080000UL, 1) == 0, "an address past the end of the flash");
    CHECK(Flash_ErasePages(0x07FFF800UL, 1) == 0, "an address below the flash");
    CHECK(Flash_ErasePages(0x20000000UL, 1) == 0, "an address in RAM");
    CHECK(erase_calls == 0, "none of them reached the controller");

    reset_flash_model();
    fail_erase_on_call = 2;
    CHECK(Flash_ErasePages(MAIN_APP_ADDR, 5) == 0, "a failure on the second page");
    CHECK(erase_calls == 2, "the loop stops at the failed page");
    CHECK(unlocks == locks && !is_unlocked, "the flash is locked after the failure");

    reset_flash_model();
    CHECK(Flash_ErasePages(MAIN_APP_ADDR, 0) == 1, "zero pages is a success");
    CHECK(erase_calls == 0, "and erases nothing");
}

/* ---- programming ----------------------------------------------------------- */
static void test_write_pads_a_partial_double_word_with_erased_bytes(void)
{
    printf("  Flash_WriteVerified: content and padding\n");
    reset_flash_model();
    uint8_t *source = low_buffer(32);
    for (int i = 0; i < 20; i++) source[i] = (uint8_t)(0x10 + i);
    CHECK(Flash_WriteVerified(MAIN_APP_ADDR, source, 20) == 1, "20 bytes");
    uint32_t off = MAIN_APP_ADDR - FLASH_BASE_ADDR;
    CHECK(memcmp(flash_mem + off, source, 20) == 0, "the 20 bytes are there");
    CHECK(flash_mem[off + 20] == 0xFF && flash_mem[off + 23] == 0xFF, "the rest of the last double-word stays erased");
    CHECK(flash_mem[off + 24] == 0xFF, "nothing is written beyond it");
    CHECK(program_calls == 3, "three double-words");
    CHECK(unlocks == 1 && locks == 1, "unlocked once, locked once");
    munmap(source, 32);
}

static void test_write_of_nothing_succeeds_without_touching_the_flash(void)
{
    printf("  Flash_WriteVerified: empty write\n");
    reset_flash_model();
    uint8_t byte = 0;
    CHECK(Flash_WriteVerified(MAIN_APP_ADDR, &byte, 0) == 1, "zero bytes");
    CHECK(program_calls == 0, "no programming");
    CHECK(unlocks == locks, "balanced lock and unlock");
}

static void test_write_over_a_page_that_was_not_erased_is_refused(void)
{
    printf("  Flash_WriteVerified: programming over data\n");
    reset_flash_model();
    memset(flash_mem + (MAIN_APP_ADDR - FLASH_BASE_ADDR), 0xAA, 16);
    uint8_t source[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(Flash_WriteVerified(MAIN_APP_ADDR, source, 8) == 0, "the controller refuses it and so does the routine");
    CHECK(program_error_not_erased == 1, "it was refused for that reason");
    CHECK(unlocks == locks && !is_unlocked, "the flash is locked again");
}

static void test_write_stops_at_the_first_refused_program_and_locks(void)
{
    printf("  Flash_WriteVerified: a refused program\n");
    reset_flash_model();
    fail_program_on_call = 2;
    uint8_t source[64];
    memset(source, 0x42, 64);
    CHECK(Flash_WriteVerified(MAIN_APP_ADDR, source, 64) == 0, "failure on the second double-word");
    CHECK(program_calls == 2, "no more programming after it");
    CHECK(unlocks == locks && !is_unlocked, "locked again");
}

static void test_write_finds_a_stuck_cell_by_reading_back(void)
{
    printf("  Flash_WriteVerified: read-back\n");
    reset_flash_model();
    stuck_cell_at_call = 2;
    uint8_t source[32];
    memset(source, 0x77, 32);
    CHECK(Flash_WriteVerified(MAIN_APP_ADDR, source, 32) == 0, "a bit that did not hold is reported");
    CHECK(unlocks == locks && !is_unlocked, "the flash is not left unlocked");
}

static void test_write_feeds_the_watchdog_on_long_writes(void)
{
    printf("  Flash_WriteVerified: watchdog on a full page\n");
    reset_flash_model();
    uint8_t *source = low_buffer(FLASH_PAGE_SIZE);
    memset(source, 0x33, FLASH_PAGE_SIZE);
    CHECK(Flash_WriteVerified(MAIN_APP_ADDR, source, FLASH_PAGE_SIZE) == 1, "a whole page");
    CHECK(iwdg_refreshes >= 2, "refreshed while programming and while reading back");
    munmap(source, FLASH_PAGE_SIZE);
}

/* ---- copy and metadata ------------------------------------------------------- */
static void fill_pattern(uint32_t addr, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) flash_mem[addr - FLASH_BASE_ADDR + i] = (uint8_t)((i * 31 + 5) & 0xFF);
}

static void test_copy_moves_the_backup_slot_to_the_main_slot_page_by_page(void)
{
    printf("  Flash_CopyRegion\n");
    reset_flash_model();
    uint32_t len = 2 * FLASH_PAGE_SIZE + 700;
    fill_pattern(BACKUP_APP_ADDR, len);
    memset(flash_mem + (MAIN_APP_ADDR - FLASH_BASE_ADDR), 0x00, 3 * FLASH_PAGE_SIZE);
    CHECK(Flash_CopyRegion(MAIN_APP_ADDR, BACKUP_APP_ADDR, len) == 1, "three pages, the last one partial");
    CHECK(memcmp(flash_mem + (MAIN_APP_ADDR - FLASH_BASE_ADDR), flash_mem + (BACKUP_APP_ADDR - FLASH_BASE_ADDR), len) == 0, "the content is identical");
    CHECK(erase_calls == 3, "one erase per destination page");
    CHECK(erase_bank[0] == FLASH_BANK_1 && erase_page[0] == 18 && erase_page[2] == 20, "destination pages 18 to 20 of bank 1");
    CHECK(heartbeat_calls == 3, "one heartbeat per page");
    CHECK(heartbeat_percent[0] == 33 && heartbeat_percent[1] == 66 && heartbeat_percent[2] == 100, "33, 66 and 100 percent");
    CHECK(heartbeat_status[0] == STATUS_COPYING && heartbeat_status[2] == STATUS_COPYING, "reported as copying");
    CHECK(flash_mem[MAIN_APP_ADDR - FLASH_BASE_ADDR + len] == 0xFF, "the end of the last page is erased, not stale");
}

static void test_copy_never_modifies_the_source_and_stops_at_a_failed_erase(void)
{
    printf("  Flash_CopyRegion: failure\n");
    reset_flash_model();
    fill_pattern(BACKUP_APP_ADDR, 3 * FLASH_PAGE_SIZE);
    static uint8_t before[3 * 2048];
    memcpy(before, flash_mem + (BACKUP_APP_ADDR - FLASH_BASE_ADDR), sizeof(before));
    fail_erase_on_call = 2;
    CHECK(Flash_CopyRegion(MAIN_APP_ADDR, BACKUP_APP_ADDR, 3 * FLASH_PAGE_SIZE) == 0, "the second erase fails");
    CHECK(heartbeat_calls == 1, "progress was reported only for the page that was copied");
    CHECK(memcmp(before, flash_mem + (BACKUP_APP_ADDR - FLASH_BASE_ADDR), sizeof(before)) == 0, "the backup slot is untouched");
}

static FirmwareMetadata_t sample_metadata(void)
{
    FirmwareMetadata_t m;
    memset(&m, 0, sizeof(m));
    m.magic = METADATA_MAGIC_VALID;
    m.state = META_STATE_APP_VALID;
    m.hardware_id = THIS_HARDWARE_ID;
    m.version_major = 0;
    m.version_minor = 2;
    m.size = 12345;
    m.crc32 = 0xDEADBEEF;
    memset(m.sha256, 0xA5, sizeof(m.sha256));
    m.counter = 7;
    return m;
}

static FirmwareMetadata_t state_number(uint32_t n)
{
    FirmwareMetadata_t m = sample_metadata();
    m.version_patch = n;
    m.size = 1000 + n;
    return m;
}

static const MetaRecord_t *log_slot(uint32_t page, uint32_t index)
{
    return (const MetaRecord_t *)(flash_mem + (METADATA_ADDR - FLASH_BASE_ADDR) + page * FLASH_PAGE_SIZE + index * META_RECORD_SIZE);
}

static uint32_t log_used(uint32_t page)
{
    uint32_t used = 0;
    for (uint32_t i = 0; i < META_RECORD_SLOTS; i++) {
        const uint8_t *bytes = (const uint8_t *)log_slot(page, i);
        for (uint32_t k = 0; k < META_RECORD_SIZE; k++) {
            if (bytes[k] != 0xFF) { used++; break; }
        }
    }
    return used;
}

static void test_the_record_has_the_size_the_layout_was_made_for(void)
{
    printf("  metadata record layout\n");
    CHECK(sizeof(FirmwareMetadata_t) == 72 && sizeof(MetaRecord_t) == META_RECORD_SIZE && META_RECORD_SIZE % 8 == 0 && META_RECORD_SLOTS == 25,
          "72 bytes of state, an 80-byte record (whole double-words), 25 to a page");
}

static void test_metadata_is_only_valid_with_its_magic_and_its_crc(void)
{
    printf("  Metadata_Read\n");
    reset_flash_model();
    FirmwareMetadata_t read;
    CHECK(Metadata_Read(&read) == 0 && read.magic == 0, "blank flash is not valid metadata");
    MetaRecord_t record;
    memset(&record, 0, sizeof(record));
    record.generation = 1;
    record.meta = sample_metadata();
    record.crc = meta_record_crc(&record);
    memcpy(flash_mem + (METADATA_ADDR - FLASH_BASE_ADDR), &record, sizeof(record));
    CHECK(Metadata_Read(&read) == 1 && memcmp(&read, &record.meta, sizeof(read)) == 0, "a written record reads back");
    record.crc ^= 1;
    memcpy(flash_mem + (METADATA_ADDR - FLASH_BASE_ADDR), &record, sizeof(record));
    CHECK(Metadata_Read(&read) == 0, "a record with a wrong CRC is refused");
    record.crc ^= 1;
    record.meta.magic ^= 1;
    record.crc = meta_record_crc(&record);
    memcpy(flash_mem + (METADATA_ADDR - FLASH_BASE_ADDR), &record, sizeof(record));
    CHECK(Metadata_Read(&read) == 0, "a record with a damaged magic is refused even with a good CRC");
}

static void test_metadata_appends_records_without_erasing(void)
{
    printf("  Metadata_EraseAndWrite: an append-only log\n");
    reset_flash_model();
    FirmwareMetadata_t read;
    for (uint32_t n = 1; n <= 5; n++) {
        FirmwareMetadata_t m = state_number(n);
        CHECK(Metadata_EraseAndWrite(&m) == 1, "a commit");
    }
    CHECK(erase_calls == 0, "five commits and not one erase");
    CHECK(log_used(0) == 5 && log_used(1) == 0, "five records in the first page");
    CHECK(Metadata_Read(&read) == 1 && read.version_patch == 5, "the state is the last record");
    CHECK(log_slot(0, 0)->generation == 1 && log_slot(0, 4)->generation == 5, "the generations count 1, 2, 3...");
    int programs = program_calls;
    FirmwareMetadata_t same = state_number(5);
    CHECK(Metadata_EraseAndWrite(&same) == 1 && program_calls == programs && erase_calls == 0, "the state that already is the last one is not written again");
    CHECK(unlocks == locks && !is_unlocked, "the flash is locked again");
}

static void test_metadata_uses_the_two_pages_in_turn(void)
{
    printf("  Metadata_EraseAndWrite: two pages in turn\n");
    reset_flash_model();
    FirmwareMetadata_t read;
    for (uint32_t n = 1; n <= META_RECORD_SLOTS; n++) {
        FirmwareMetadata_t m = state_number(n);
        Metadata_EraseAndWrite(&m);
    }
    CHECK(log_used(0) == META_RECORD_SLOTS && erase_calls == 0, "25 commits fill the first page without an erase");
    FirmwareMetadata_t next = state_number(26);
    CHECK(Metadata_EraseAndWrite(&next) == 1, "the 26th commit");
    CHECK(erase_calls == 1 && erase_bank[0] == FLASH_BANK_1 && erase_page[0] == 17, "erases the second page (page 17 of bank 1) and nothing else");
    CHECK(log_used(0) == META_RECORD_SLOTS && log_used(1) == 1, "the first page keeps its records, the new one is the first of the second");
    CHECK(Metadata_Read(&read) == 1 && read.version_patch == 26 && log_slot(1, 0)->generation == 26, "the generations go on across the pages");
    for (uint32_t n = 27; n <= 26 + META_RECORD_SLOTS - 1; n++) {
        FirmwareMetadata_t m = state_number(n);
        Metadata_EraseAndWrite(&m);
    }
    CHECK(log_used(1) == META_RECORD_SLOTS && erase_calls == 1, "the second page fills");
    FirmwareMetadata_t again = state_number(51);
    CHECK(Metadata_EraseAndWrite(&again) == 1 && erase_calls == 2 && erase_page[1] == 16, "the next commit goes back to the first page (page 16), erased");
    CHECK(Metadata_Read(&read) == 1 && read.version_patch == 51 && log_used(0) == 1 && log_used(1) == META_RECORD_SLOTS, "and it is the state");
}

static void test_a_power_cut_in_the_middle_of_a_record_keeps_the_previous_state(void)
{
    printf("  Metadata_EraseAndWrite: a cut at every double-word of a record\n");
    int all_kept = 1, all_retried = 1;
    for (int cut = 1; cut <= 10; cut++) {
        reset_flash_model();
        FirmwareMetadata_t a = state_number(1), b = state_number(2), read;
        Metadata_EraseAndWrite(&a);
        fail_program_on_call = program_calls + cut;
        uint8_t ok = Metadata_EraseAndWrite(&b);
        fail_program_on_call = 0;
        if (ok || !Metadata_Read(&read) || read.version_patch != 1 || read.size != 1001) all_kept = 0;
        if (!Metadata_EraseAndWrite(&b) || !Metadata_Read(&read) || read.version_patch != 2 || read.counter != 7) all_retried = 0;
    }
    CHECK(all_kept, "a cut at any of the ten double-words leaves the previous state in force");
    CHECK(all_retried, "after the cut the same commit succeeds on the next free slot");
}

static void test_a_cut_while_the_page_changes_never_loses_the_state(void)
{
    printf("  Metadata_EraseAndWrite: a cut while the log changes page\n");
    FirmwareMetadata_t read;
    reset_flash_model();
    for (uint32_t n = 1; n <= META_RECORD_SLOTS; n++) {
        FirmwareMetadata_t m = state_number(n);
        Metadata_EraseAndWrite(&m);
    }
    fail_erase_on_call = 1;
    FirmwareMetadata_t next = state_number(26);
    CHECK(Metadata_EraseAndWrite(&next) == 0, "the erase of the other page fails");
    fail_erase_on_call = 0;
    CHECK(Metadata_Read(&read) == 1 && read.version_patch == META_RECORD_SLOTS, "the full page and its newest record are still the state");

    int all_kept = 1;
    for (int cut = 1; cut <= 10; cut++) {
        reset_flash_model();
        for (uint32_t n = 1; n <= META_RECORD_SLOTS; n++) {
            FirmwareMetadata_t m = state_number(n);
            Metadata_EraseAndWrite(&m);
        }
        fail_program_on_call = program_calls + cut;
        FirmwareMetadata_t after = state_number(26);
        uint8_t ok = Metadata_EraseAndWrite(&after);
        fail_program_on_call = 0;
        if (ok || !Metadata_Read(&read) || read.version_patch != META_RECORD_SLOTS) all_kept = 0;
        if (!Metadata_EraseAndWrite(&after) || !Metadata_Read(&read) || read.version_patch != 26) all_kept = 0;
    }
    CHECK(all_kept, "a cut at any double-word of the first record of the new page leaves the old state, and the retry works");
}

static void test_a_damaged_record_is_skipped_and_the_newest_valid_one_wins(void)
{
    printf("  Metadata_Read: a damaged record in the log\n");
    reset_flash_model();
    FirmwareMetadata_t read;
    for (uint32_t n = 1; n <= 4; n++) {
        FirmwareMetadata_t m = state_number(n);
        Metadata_EraseAndWrite(&m);
    }
    flash_mem[METADATA_ADDR - FLASH_BASE_ADDR + 3 * META_RECORD_SIZE + 20] = 0x00;   /* spoil the fourth record: a bit clears, its CRC no longer matches */
    CHECK(Metadata_Read(&read) == 1 && read.version_patch == 3, "the previous record stays in force");
}

int main(void)
{
    printf("HYDRA-UMC host logic tests - G474 bootloader SHA-256, CRC32, flash routines and metadata log\n");

    flash_mem = mmap((void *)FLASH_BASE_ADDR, FLASH_TOTAL, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (flash_mem == MAP_FAILED || (uintptr_t)flash_mem != FLASH_BASE_ADDR) {
        printf("  SKIP: cannot map the flash address range on this host\n");
        return 0;
    }

    test_sha256_padding_boundaries();
    test_the_digest_of_a_header_and_an_image_matches_the_reference();
    test_sha256_buffer_and_the_split_between_prefix_and_region();
    test_the_signature_check_matches_rfc_8032_and_the_reference();
    test_the_signature_check_keeps_the_watchdog_fed();
    test_sha256_feeds_the_watchdog_on_a_large_region();
    test_constant_time_compare();
    test_crc32();
    test_erase_maps_each_address_to_bank_and_page();
    test_erase_crosses_the_bank_boundary_and_feeds_the_watchdog_per_page();
    test_erase_only_touches_the_requested_page();
    test_erase_refuses_what_it_cannot_do_safely();
    test_write_pads_a_partial_double_word_with_erased_bytes();
    test_write_of_nothing_succeeds_without_touching_the_flash();
    test_write_over_a_page_that_was_not_erased_is_refused();
    test_write_stops_at_the_first_refused_program_and_locks();
    test_write_finds_a_stuck_cell_by_reading_back();
    test_write_feeds_the_watchdog_on_long_writes();
    test_copy_moves_the_backup_slot_to_the_main_slot_page_by_page();
    test_copy_never_modifies_the_source_and_stops_at_a_failed_erase();
    test_the_record_has_the_size_the_layout_was_made_for();
    test_metadata_is_only_valid_with_its_magic_and_its_crc();
    test_metadata_appends_records_without_erasing();
    test_metadata_uses_the_two_pages_in_turn();
    test_a_power_cut_in_the_middle_of_a_record_keeps_the_previous_state();
    test_a_cut_while_the_page_changes_never_loses_the_state();
    test_a_damaged_record_is_skipped_and_the_newest_valid_one_wins();

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all G474 bootloader host tests passed\n");
    return 0;
}

#else /* not Linux on x86-64 */

int main(void)
{
    printf("HYDRA-UMC host logic tests - G474 bootloader: skipped (needs Linux on x86-64)\n");
    return 0;
}

#endif
