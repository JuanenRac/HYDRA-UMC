/* =============================================================================
 * HYDRA-UMC Firmware - Host logic tests for the G474 bootloader's SHA-256, HMAC,
 * Ed25519 check, CRC32 and flash erase/program/copy/metadata routines
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * The two bootloader sources are compiled as they are (included below) against
 * tests/stubs/stm32g4xx_hal.h; this file plays the flash controller (a 512 KB
 * array mapped at the real flash address, erase by bank and page, programming
 * only over erased double-words) and counts the watchdog refreshes.
 *
 *  - HMAC-SHA256 against the RFC 4231 vectors and every padding boundary of the
 *    SHA-256 block, and the constant-time comparison.
 *  - SHA-256 of header || image against the host reference, and the Ed25519 check
 *    (update_signature.c, linked with the vendored TweetNaCl) against the RFC 8032
 *    vectors and the signatures tools/image_header.py made.
 *  - CRC32 against the zlib value.
 *  - Flash_ErasePages: bank/page of each address, the dual-bank refusal, a
 *    failure in the middle, one watchdog refresh per page.
 *  - Flash_WriteVerified: padding of a partial double-word, a refused program, a
 *    stuck cell found by the read-back, the lock after every path.
 *  - Flash_CopyRegion and Metadata_EraseAndWrite on top of them.
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

static uint32_t hmac_of(const uint8_t *key, uint32_t key_len, const uint8_t *data, uint32_t len, uint8_t out[32])
{
    uint8_t *low = low_buffer(len);
    if (len) memcpy(low, data, len);
    hmac_sha256_flash_region(key, key_len, (uint32_t)(uintptr_t)low, len, out);
    munmap(low, len ? len : 1);
    return len;
}

static int hex_equals(const uint8_t *bytes, const char *hex)
{
    char text[65];
    for (int i = 0; i < 32; i++) sprintf(&text[i * 2], "%02x", bytes[i]);
    return strcmp(text, hex) == 0;
}

/* ---- HMAC ---------------------------------------------------------------- */
static void test_hmac_rfc4231(void)
{
    printf("  HMAC-SHA256, RFC 4231 vectors\n");
    uint8_t out[32];
    uint8_t key[131];
    uint8_t data[150];

    memset(key, 0x0b, 20);
    hmac_of(key, 20, (const uint8_t *)"Hi There", 8, out);
    CHECK(hex_equals(out, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"), "test case 1");

    hmac_of((const uint8_t *)"Jefe", 4, (const uint8_t *)"what do ya want for nothing?", 28, out);
    CHECK(hex_equals(out, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"), "test case 2");

    memset(key, 0xaa, 20);
    memset(data, 0xdd, 50);
    hmac_of(key, 20, data, 50, out);
    CHECK(hex_equals(out, "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe"), "test case 3");

    for (int i = 0; i < 25; i++) key[i] = (uint8_t)(i + 1);
    memset(data, 0xcd, 50);
    hmac_of(key, 25, data, 50, out);
    CHECK(hex_equals(out, "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b"), "test case 4");

    memset(key, 0xaa, 131);
    hmac_of(key, 131, (const uint8_t *)"Test Using Larger Than Block-Size Key - Hash Key First", 54, out);
    CHECK(hex_equals(out, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"), "test case 6 (key longer than the block)");

    const char *long_text = "This is a test using a larger than block-size key and a larger than block-size data. The key needs to be hashed before being used by the HMAC algorithm.";
    hmac_of(key, 131, (const uint8_t *)long_text, (uint32_t)strlen(long_text), out);
    CHECK(hex_equals(out, "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2"), "test case 7 (long key and long data)");
}

static void test_hmac_padding_boundaries(void)
{
    printf("  HMAC-SHA256, every padding boundary of the 64-byte block\n");
    static const struct { uint32_t len; const char *expected; } cases[] = {
        {0,    "d38b42096d80f45f826b44a9d5607de72496a415d3f4a1a8c88e3bb9da8dc1cb"},
        {1,    "2a24d008789d3c74daf5e02636c675df8f09ec5e740c1bdf6305f9261f7b1c32"},
        {55,   "e66a4fff985be53712fd4aa0892c3b80983e902b6caa3e98ed1186c0a89c7afe"},
        {56,   "b2727065101957bf27ce9b50104c6062e0c376b71df36cb336d26948103afd3b"},
        {57,   "182e429b428c7cbfcd036ea01e92a0c04025dc81ddf2619921d1a056963e4ce5"},
        {63,   "47604389fafb6ab7907b9c09e23589ff0203384382bf7bddebd6130a37ffa379"},
        {64,   "b46ff212a3a07583c8dd50c674c6cff3558e6f7b5b97081f7c73101a2b0086fb"},
        {65,   "006baa702522d812e0ea3ab7d7f7368b0f87a1abf446187d6ae02d0d8d450dcf"},
        {119,  "e5012dfb2a972eda915425f71385b14565a9fb0de92f0bd37705574119694a53"},
        {120,  "aad486123ad591a6e8376bd81ae6f479ab9a2a309f08b24d65220e2572e5b45f"},
        {121,  "fc393177c63a8c5d9c2bb8a565f21509b44ca1e5d3962b4301c05005cbf36e61"},
        {127,  "737bf24b356aeed0a29bbcf886a0aea0b0e02ca5a1de3fb0602620e826f42878"},
        {128,  "32580bd113595450aeaa291ce4db3db6b529206e7b04e16bd0408224e29d61b6"},
        {129,  "59829dbfbf4e8c8680f11979c7d338f7c6331caa5eabc072bfbab1cf87a737bb"},
        {1000, "13f22d9be5636c710a12699e36a0390622d622f876e5aec2ddd33db4862cfd4f"},
    };
    uint8_t key[32];
    uint8_t data[1000];
    uint8_t out[32];
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)i;
    for (int i = 0; i < 1000; i++) data[i] = (uint8_t)((i * 7 + 3) & 0xFF);
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        char what[48];
        snprintf(what, sizeof(what), "%u bytes", cases[c].len);
        hmac_of(key, 32, data, cases[c].len, out);
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
    CHECK(hmac_constant_time_compare(out, VEC_DIGEST, 32) == 1, "the digest the reference made for the image matches");

    /* one changed bit anywhere in the header or the image is another digest */
    uint8_t header[sizeof(VEC_HEADER)];
    for (size_t i = 0; i < sizeof(header); i += 9) {
        memcpy(header, VEC_HEADER, sizeof(header));
        header[i] ^= 0x01;
        sha256_prefixed_flash_region(header, sizeof(header), BACKUP_APP_ADDR, VEC_IMAGE_SIZE, out);
        CHECK(hmac_constant_time_compare(out, VEC_DIGEST, 32) == 0, "a changed header byte changes the digest");
    }
    for (uint32_t i = 0; i < VEC_IMAGE_SIZE; i += 61) {
        flash_mem[BACKUP_APP_ADDR - FLASH_BASE_ADDR + i] ^= 0x01;
        sha256_prefixed_flash_region(VEC_HEADER, sizeof(VEC_HEADER), BACKUP_APP_ADDR, VEC_IMAGE_SIZE, out);
        CHECK(hmac_constant_time_compare(out, VEC_DIGEST, 32) == 0, "a changed image byte changes the digest");
        flash_mem[BACKUP_APP_ADDR - FLASH_BASE_ADDR + i] ^= 0x01;
    }
    /* the image alone, as the old flow covered, is not the digest of the signed image */
    sha256_prefixed_flash_region(NULL, 0, BACKUP_APP_ADDR, VEC_IMAGE_SIZE, out);
    CHECK(hmac_constant_time_compare(out, VEC_DIGEST, 32) == 0, "the SHA-256 of the image alone is not the digest of the signed image");
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
        CHECK(hmac_constant_time_compare(whole, parts, 32) == 1, what);
        munmap(low, 200 - split + 1);
    }
}

static void test_hmac_prefix_and_region_are_one_message_whatever_the_split(void)
{
    printf("  HMAC-SHA256: the split between the prefix and the region does not matter\n");
    reset_flash_model();
    uint8_t key[32];
    uint8_t data[200];
    uint8_t whole[32], parts[32];
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)(i * 3 + 1);
    for (int i = 0; i < 200; i++) data[i] = (uint8_t)(i * 5 + 7);
    hmac_of(key, 32, data, 200, whole);
    static const uint32_t splits[] = {0, 1, 55, 56, 63, 64, 65, 127, 128, 199, 200};
    for (size_t k = 0; k < sizeof(splits) / sizeof(splits[0]); k++) {
        uint32_t split = splits[k];
        uint8_t *low = low_buffer(200 - split);
        memcpy(low, data + split, 200 - split);
        hmac_sha256_prefixed_flash_region(key, 32, data, split, (uint32_t)(uintptr_t)low, 200 - split, parts);
        char what[48];
        snprintf(what, sizeof(what), "prefix of %u bytes", split);
        CHECK(hmac_constant_time_compare(whole, parts, 32) == 1, what);
        munmap(low, 200 - split);
    }
    hmac_sha256_buffer(key, 32, data, 200, parts);
    CHECK(hmac_constant_time_compare(whole, parts, 32) == 1, "the buffer form equals the flash form");
    hmac_sha256_buffer(key, 32, data, 0, parts);
    uint8_t empty[32];
    hmac_of(key, 32, data, 0, empty);
    CHECK(hmac_constant_time_compare(empty, parts, 32) == 1, "an empty message");
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

static void test_the_development_public_key_is_in_the_bootloader(void)
{
    printf("  development public key of the bootloader\n");
    int all_zero = 1;
    for (int i = 0; i < 32; i++) {
        if (UPDATE_PUBLIC_KEY[i] != 0) all_zero = 0;
    }
    CHECK(all_zero == 0, "the key of the bootloader is not empty");
    CHECK(memcmp(UPDATE_PUBLIC_KEY, VEC_PUBLIC_KEY, 32) != 0, "and it is not the key of the test vectors");
}

static void test_hmac_feeds_the_watchdog_and_reads_the_region_once(void)
{
    printf("  HMAC-SHA256 keeps the watchdog fed on a large region\n");
    reset_flash_model();
    uint8_t key[32] = {0};
    uint8_t out[32];
    uint32_t len = 100000;
    uint8_t *data = calloc(1, len);
    hmac_of(key, 32, data, len, out);
    free(data);
    CHECK(iwdg_refreshes >= (int)(len / 4096U), "at least one refresh per 4096 bytes");
}

static void test_constant_time_compare(void)
{
    printf("  constant-time comparison\n");
    uint8_t a[32], b[32];
    memset(a, 0x5a, 32);
    memcpy(b, a, 32);
    CHECK(hmac_constant_time_compare(a, b, 32) == 1, "equal buffers");
    for (int i = 0; i < 32; i++) {
        b[i] ^= 0x01;
        CHECK(hmac_constant_time_compare(a, b, 32) == 0, "a single changed bit anywhere is a mismatch");
        b[i] ^= 0x01;
    }
    CHECK(hmac_constant_time_compare(a, b, 0) == 1, "nothing to compare is equal");
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
    memset(m.hmac, 0xA5, sizeof(m.hmac));
    return m;
}

static void test_metadata_is_only_valid_with_its_magic(void)
{
    printf("  Metadata_Read\n");
    reset_flash_model();
    FirmwareMetadata_t read;
    CHECK(Metadata_Read(&read) == 0, "blank flash is not valid metadata");
    FirmwareMetadata_t m = sample_metadata();
    memcpy(flash_mem + (METADATA_ADDR - FLASH_BASE_ADDR), &m, sizeof(m));
    CHECK(Metadata_Read(&read) == 1 && memcmp(&read, &m, sizeof(m)) == 0, "a written record reads back");
    m.magic ^= 1;
    memcpy(flash_mem + (METADATA_ADDR - FLASH_BASE_ADDR), &m, sizeof(m));
    CHECK(Metadata_Read(&read) == 0, "a record with a damaged magic is refused");
}

static void test_metadata_write_skips_an_identical_record_and_replaces_a_different_one(void)
{
    printf("  Metadata_EraseAndWrite\n");
    reset_flash_model();
    FirmwareMetadata_t m = sample_metadata();
    FirmwareMetadata_t read;
    CHECK(Metadata_EraseAndWrite(&m) == 1, "first write");
    CHECK(erase_calls == 1 && erase_bank[0] == FLASH_BANK_1 && erase_page[0] == 16, "erased page 16 of bank 1");
    CHECK(Metadata_Read(&read) == 1 && memcmp(&read, &m, sizeof(m)) == 0, "it reads back");

    int erases = erase_calls, programs = program_calls;
    CHECK(Metadata_EraseAndWrite(&m) == 1, "the same record again");
    CHECK(erase_calls == erases && program_calls == programs, "no erase and no programming for an identical record");

    m.boot_attempts = 2;
    CHECK(Metadata_EraseAndWrite(&m) == 1, "a changed record");
    CHECK(erase_calls == erases + 1, "is written after an erase");
    CHECK(Metadata_Read(&read) == 1 && read.boot_attempts == 2, "the new value is there");
}

static void test_metadata_write_fails_when_the_erase_fails(void)
{
    printf("  Metadata_EraseAndWrite: failure\n");
    reset_flash_model();
    FirmwareMetadata_t m = sample_metadata();
    fail_erase_on_call = 1;
    CHECK(Metadata_EraseAndWrite(&m) == 0, "the erase fails");
    CHECK(program_calls == 0, "and nothing is programmed");
}

int main(void)
{
    printf("HYDRA-UMC host logic tests - G474 bootloader HMAC, CRC32 and flash routines\n");

    flash_mem = mmap((void *)FLASH_BASE_ADDR, FLASH_TOTAL, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (flash_mem == MAP_FAILED || (uintptr_t)flash_mem != FLASH_BASE_ADDR) {
        printf("  SKIP: cannot map the flash address range on this host\n");
        return 0;
    }

    test_hmac_rfc4231();
    test_hmac_padding_boundaries();
    test_the_digest_of_a_header_and_an_image_matches_the_reference();
    test_sha256_buffer_and_the_split_between_prefix_and_region();
    test_hmac_prefix_and_region_are_one_message_whatever_the_split();
    test_the_signature_check_matches_rfc_8032_and_the_reference();
    test_the_signature_check_keeps_the_watchdog_fed();
    test_the_development_public_key_is_in_the_bootloader();
    test_hmac_feeds_the_watchdog_and_reads_the_region_once();
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
    test_metadata_is_only_valid_with_its_magic();
    test_metadata_write_skips_an_identical_record_and_replaces_a_different_one();
    test_metadata_write_fails_when_the_erase_fails();

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
