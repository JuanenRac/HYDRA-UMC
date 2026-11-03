/* =============================================================================
 * HYDRA-UMC Firmware - Host logic tests of the trust table of the bootloaders
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * bootloader_trust.c is compiled and linked with the vendored TweetNaCl (which hashes the key ids), once per bootloader directory (the three
 * copies must behave the same) and in two flavours: with -DHYDRA_TRUST_DEV, the bench table, and without it (-DHYDRA_TEST_RELEASE), the table
 * of an official build. The table cases are the ones the host reference (tools/image_trust.py) made, in tests/image_format2_vectors.h.
 */
#include <stdio.h>
#include <string.h>

#include "bootloader_trust.h"
#include "image_format2_vectors.h"
#ifdef HYDRA_TEST_RELEASE
#include "hydra_trust_release.h"
#else
#include "hydra_trust_dev.h"
#endif

extern const uint8_t HYDRA_TRUST_TABLE[];

/* The vendored TweetNaCl links against this for key generation, which nothing here does. */
void randombytes(unsigned char *buffer, unsigned long long length);
void randombytes(unsigned char *buffer, unsigned long long length)
{
    for (unsigned long long i = 0; i < length; i++) buffer[i] = 0;
}

static int failures;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("  FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
            failures++; \
        } \
    } while (0)

static void refresh_crc(uint8_t *table, uint32_t size)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < size - 4u; i++) {
        crc ^= table[i];
        for (int bit = 0; bit < 8; bit++) crc = (crc & 1u) ? (0xEDB88320u ^ (crc >> 1)) : (crc >> 1);
    }
    crc ^= 0xFFFFFFFFu;
    table[size - 4u] = (uint8_t)crc;
    table[size - 3u] = (uint8_t)(crc >> 8);
    table[size - 2u] = (uint8_t)(crc >> 16);
    table[size - 1u] = (uint8_t)(crc >> 24);
}

static void test_the_table_cases_of_the_host_reference(void)
{
    printf("  the table cases of the host reference\n");
    int wrong = 0;
    for (unsigned n = 0; n < F2_TABLE_CASE_COUNT; n++) {
        const Format2TableCase *c = &F2_TABLE_CASES[n];
        TrustKeys_t keys;
        uint8_t count = Trust_Parse(c->table, c->table_len, &keys);
        int ok = c->valid ? (count == c->trusted_count) : (count == 0 && keys.count == 0);
        for (unsigned k = 0; ok && k < c->trusted_count; k++) {
            if (memcmp(keys.keys[k], c->trusted[k], 32) != 0) ok = 0;
        }
        if (!ok) {
            printf("    case %u (%s): %u trusted keys\n", n, c->name, (unsigned)count);
            wrong++;
        }
    }
    CHECK(wrong == 0, "every table case gets the answer of the host reference");
}

static void test_any_single_flipped_bit_closes_the_table(void)
{
    printf("  a flipped bit anywhere in a table\n");
    const Format2TableCase *two = &F2_TABLE_CASES[1];   /* current and next */
    TrustKeys_t keys;
    CHECK(Trust_Parse(two->table, two->table_len, &keys) == 2, "the untouched table trusts both keys");
    int none_survives = 1;
    for (uint32_t byte = 0; byte < two->table_len; byte++) {
        for (int bit = 0; bit < 8; bit++) {
            uint8_t copy[100];
            memcpy(copy, two->table, two->table_len);
            copy[byte] ^= (uint8_t)(1u << bit);
            if (Trust_Parse(copy, two->table_len, &keys) != 0) none_survives = 0;
        }
    }
    CHECK(none_survives, "no single flipped bit leaves a trusted key");
    CHECK(Trust_Parse(two->table, two->table_len - 1u, &keys) == 0, "a table cut short trusts nobody");
    CHECK(Trust_Parse(NULL, 100, &keys) == 0, "no table trusts nobody");
}

static void test_the_states_of_a_key_are_checked(void)
{
    printf("  the states of a key\n");
    const Format2TableCase *one = &F2_TABLE_CASES[0];   /* one current key, 56 bytes */
    TrustKeys_t keys;
    uint8_t table[100];
    static const uint8_t bad_flags[] = {0, 3, 5, 6, 7, 8, 0x81};
    int all_refused = 1;
    for (unsigned i = 0; i < sizeof bad_flags; i++) {
        memcpy(table, one->table, one->table_len);
        table[48] = bad_flags[i];
        refresh_crc(table, one->table_len);
        if (Trust_Parse(table, one->table_len, &keys) != 0) all_refused = 0;
    }
    CHECK(all_refused, "no state, two states or an unknown state closes the table (even with a good CRC)");
    memcpy(table, one->table, one->table_len);
    table[8] ^= 1;
    refresh_crc(table, one->table_len);
    CHECK(Trust_Parse(table, one->table_len, &keys) == 0, "a key id that is not the hash of its key closes the table");
    memcpy(table, one->table, one->table_len);
    table[49] = 1;
    refresh_crc(table, one->table_len);
    CHECK(Trust_Parse(table, one->table_len, &keys) == 0, "padding that is not zero closes the table");
    memcpy(table, one->table, one->table_len);
    table[4] = 2;
    refresh_crc(table, one->table_len);
    CHECK(Trust_Parse(table, one->table_len, &keys) == 0, "an unknown schema closes the table");
}

static void test_the_compiled_table(void)
{
    printf("  the table compiled into this build\n");
    static const uint8_t compiled[] = { HYDRA_TRUST_TABLE_BYTES };
    CHECK(sizeof compiled == HYDRA_TRUST_TABLE_SIZE && memcmp(HYDRA_TRUST_TABLE, compiled, sizeof compiled) == 0, "it is the table of the generated header");
    CHECK(Trust_MinCounter() == (uint32_t)HYDRA_TRUST_MIN_COUNTER, "the compiled floor of the counter is the one of the public file");
    const TrustKeys_t *active = Trust_Active();
#ifdef HYDRA_TEST_RELEASE
    if (HYDRA_TRUST_TABLE_SIZE == 12u) {
        CHECK(active->count == 0, "no image-signing key is provisioned: the official bootloader trusts nobody and installs nothing");
    } else {
        CHECK(active->count >= 1, "the release table holds the image-signing key");
    }
    /* the development key (the table of the bench build) must never be in an official table */
    static const uint8_t dev_public[32] = {0x3F, 0x50, 0x8A, 0x29, 0xD3, 0x5D, 0x9F, 0x33, 0x85, 0xFD, 0xD5, 0x6B, 0x33, 0xD3, 0xD1, 0x21,
                                           0x9E, 0x35, 0xBB, 0x61, 0x80, 0x60, 0x9A, 0x83, 0x8F, 0x6D, 0x44, 0xBA, 0xCF, 0xAF, 0x80, 0xB2};
    int dev = 0;
    for (uint8_t i = 0; i < active->count; i++) {
        if (memcmp(active->keys[i], dev_public, 32) == 0) dev = 1;
    }
    CHECK(!dev, "an official bootloader never trusts the public development key");
#else
    CHECK(active->count == 1 && active->flags[0] == TRUST_FLAG_CURRENT, "the bench build trusts one current key");
    CHECK(memcmp(HYDRA_TRUST_TABLE, "TKEY", 4) == 0, "and its table starts with its magic");
#endif
}

int main(void)
{
#ifdef HYDRA_TEST_RELEASE
    printf("HYDRA-UMC host logic tests - the release trust table\n");
#else
    printf("HYDRA-UMC host logic tests - the trust table\n");
#endif
    test_the_table_cases_of_the_host_reference();
    test_any_single_flipped_bit_closes_the_table();
    test_the_states_of_a_key_are_checked();
    test_the_compiled_table();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all trust table host tests passed\n");
    return 0;
}
