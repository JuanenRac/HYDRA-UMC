/* =============================================================================
 * HYDRA-UMC Firmware - src/ui_h757/test/ui_pin_test.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Host test of the store of the operator PIN in C (src/ui_h757/core/ui_pin.c) against the Python reference (tools/ui_pin.py): the hashes, the bytes of the F-RAM after a PIN is
 * set, the results of a list of attempts, a write cut by the power at every byte, every flipped byte of a record, and the wrap of the sequence number. Evidence: simulator.
 *
 *   cc -std=c11 -Wall -Wextra -DUI_PIN_COUNT_BLOCKS -Isrc/ui_h757/core -Isrc/ui_h757/test -o ui_pin_test src/ui_h757/test/ui_pin_test.c src/ui_h757/core/ui_pin.c
 */
#include <stdio.h>
#include <string.h>

#include "ui_pin.h"
#include "ui_pin_vectors.h"

typedef struct {
    uint8_t data[2 * UI_PIN_RECORD_BYTES];
    int32_t cut_after; /* bytes still to be written before the power goes; -1 = never */
} fram_t;

static int fram_read(void *context, uint32_t offset, uint8_t *data, uint32_t length)
{
    const fram_t *f = (const fram_t *)context;
    if (offset + length > sizeof(f->data)) {
        return -1;
    }
    memcpy(data, f->data + offset, length);
    return 0;
}

static int fram_write(void *context, uint32_t offset, const uint8_t *data, uint32_t length)
{
    fram_t *f = (fram_t *)context;
    if (offset + length > sizeof(f->data)) {
        return -1;
    }
    for (uint32_t i = 0; i < length; i++) {
        if (f->cut_after >= 0) {
            if (f->cut_after == 0) {
                f->cut_after = -1;
                return -1;
            }
            f->cut_after--;
        }
        f->data[offset + i] = data[i];
    }
    return 0;
}

static void blank(fram_t *f)
{
    memset(f->data, 0xFF, sizeof(f->data));
    f->cut_after = -1;
}

static void open_store(ui_pin_store_t *store, fram_t *f, uint32_t iterations)
{
    ui_pin_ports_t ports;
    ports.read = fram_read;
    ports.write = fram_write;
    ports.context = f;
    ui_pin_init(store, &ports, iterations, 4, 8);
}

static const uint8_t k_salt_a[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
static const uint8_t k_salt_b[16] = {15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0};

static int vectors(void)
{
    int failures = 0;
    uint8_t out[32];
    static const uint8_t check[9] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    if (ui_crc32(check, 9) != 0xCBF43926u) {
        printf("  FAIL: the CRC-32 of the check string\n");
        failures++;
    }
    for (unsigned i = 0; i < UI_VEC_PBKDF2_COUNT; i++) {
        const ui_vec_pbkdf2_t *v = &ui_vec_pbkdf2[i];
        ui_sha256_blocks = 0;
        ui_pbkdf2_sha256((const uint8_t *)v->pin, (uint32_t)strlen(v->pin), v->salt, 16, v->iterations, out);
        if (memcmp(out, v->hash, 32) != 0) {
            printf("  FAIL: PBKDF2 vector %u\n", i);
            failures++;
        }
        if (ui_sha256_blocks != 2u * v->iterations + 2u) {
            printf("  FAIL: %u compressions for %u iterations (expected %u)\n", (unsigned)ui_sha256_blocks, (unsigned)v->iterations, (unsigned)(2u * v->iterations + 2u));
            failures++;
        }
    }
    {
        fram_t f;
        ui_pin_store_t store;
        blank(&f);
        open_store(&store, &f, 1000);
        for (unsigned i = 0; i < UI_VEC_SET_COUNT; i++) {
            const ui_vec_set_t *v = &ui_vec_sets[i];
            if (ui_pin_set(&store, v->pin, v->salt) != UI_PIN_SET_OK || memcmp(f.data, v->fram, sizeof(f.data)) != 0) {
                printf("  FAIL: the bytes of the F-RAM after set %u differ from the reference\n", i);
                failures++;
            }
        }
    }
    {
        fram_t f;
        ui_pin_store_t store;
        blank(&f);
        open_store(&store, &f, 1000);
        ui_pin_set(&store, "4821", k_salt_a);
        for (unsigned i = 0; i < UI_VEC_ATTEMPT_COUNT; i++) {
            const ui_vec_attempt_t *v = &ui_vec_attempts[i];
            ui_pin_result_t result = ui_pin_verify(&store, v->pin);
            if ((unsigned)result != v->result || ui_pin_failures(&store) != v->failures || ui_crc32(f.data, sizeof(f.data)) != v->fram_crc) {
                printf("  FAIL: attempt %u (%s): result %d failures %u\n", i, v->pin, (int)result, (unsigned)ui_pin_failures(&store));
                failures++;
            }
        }
    }
    ui_sha256_blocks = 0;
    ui_pbkdf2_sha256((const uint8_t *)"4821", 4, k_salt_a, 16, 1000, out);
    printf("  compressions for 1000 iterations: %u\n", (unsigned)ui_sha256_blocks);
    printf("  vectors: %u hashes, %u sets, %u attempts\n", (unsigned)UI_VEC_PBKDF2_COUNT, (unsigned)UI_VEC_SET_COUNT, (unsigned)UI_VEC_ATTEMPT_COUNT);
    return failures;
}

static int states_and_refusals(void)
{
    int failures = 0;
    fram_t f;
    ui_pin_store_t store;
    const char *refused[] = {"123", "123456789", "12a4", "1111", "", "\xd9\xa1\xd9\xa2\xd9\xa3\xd9\xa4"};
    blank(&f);
    open_store(&store, &f, 1000);
    if (ui_pin_state(&store) != UI_PIN_STATE_UNSET || ui_pin_verify(&store, "1234") != UI_PIN_UNAVAILABLE) {
        printf("  FAIL: a new store has no PIN and accepts none\n");
        failures++;
    }
    for (unsigned i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        if (ui_pin_set(&store, refused[i], k_salt_a) != UI_PIN_SET_BAD_PIN) {
            printf("  FAIL: a PIN of the wrong kind was accepted (%u)\n", i);
            failures++;
        }
    }
    if (ui_pin_set(&store, "4821", 0) != UI_PIN_SET_BAD_SALT) {
        printf("  FAIL: no salt\n");
        failures++;
    }
    ui_pin_set(&store, "4821", k_salt_a);
    if (ui_pin_state(&store) != UI_PIN_STATE_SET || ui_pin_verify(&store, "4821") != UI_PIN_OK || ui_pin_verify(&store, "4822") != UI_PIN_BAD) {
        printf("  FAIL: set, right and wrong\n");
        failures++;
    }
    memset(f.data, 0, sizeof(f.data));
    if (ui_pin_state(&store) != UI_PIN_STATE_CORRUPT || ui_pin_verify(&store, "4821") != UI_PIN_UNAVAILABLE) {
        printf("  FAIL: garbage must read as corrupt and let nothing in\n");
        failures++;
    }
    ui_pin_set(&store, "4821", k_salt_a);
    if (ui_pin_erase(&store) != 0 || ui_pin_state(&store) != UI_PIN_STATE_UNSET) {
        printf("  FAIL: the erase\n");
        failures++;
    }
    return failures;
}

static int restart_and_counter(void)
{
    int failures = 0;
    fram_t f;
    ui_pin_store_t store;
    ui_pin_store_t again;
    blank(&f);
    open_store(&store, &f, 1000);
    ui_pin_set(&store, "4821", k_salt_a);
    for (uint32_t expected = 1; expected <= 3; expected++) {
        if (ui_pin_verify(&store, "0000") != UI_PIN_BAD || ui_pin_failures(&store) != expected) {
            printf("  FAIL: the failure counter %u\n", (unsigned)expected);
            failures++;
        }
    }
    open_store(&again, &f, 1000); /* the power came back */
    if (ui_pin_failures(&again) != 3 || ui_pin_verify(&again, "4821") != UI_PIN_OK || ui_pin_failures(&again) != 0) {
        printf("  FAIL: the counter must survive a restart and clear after a right PIN\n");
        failures++;
    }
    return failures;
}

static int torn_writes(void)
{
    int failures = 0;
    for (int32_t cut = 0; cut <= (int32_t)UI_PIN_RECORD_BYTES; cut += 3) {
        fram_t f;
        ui_pin_store_t store;
        ui_pin_store_t fresh;
        int old_ok, new_ok;
        blank(&f);
        open_store(&store, &f, 1000);
        ui_pin_set(&store, "4821", k_salt_a);
        f.cut_after = cut;
        (void)ui_pin_set(&store, "9376", k_salt_b);
        f.cut_after = -1;
        open_store(&fresh, &f, 1000);
        old_ok = ui_pin_verify(&fresh, "4821") == UI_PIN_OK;
        new_ok = ui_pin_verify(&fresh, "9376") == UI_PIN_OK;
        if (!(old_ok || new_ok) || (old_ok && new_ok)) {
            printf("  FAIL: cut after %d: old %d new %d\n", (int)cut, old_ok, new_ok);
            failures++;
        }
    }
    {
        fram_t f;
        ui_pin_store_t store;
        blank(&f);
        open_store(&store, &f, 1000);
        ui_pin_set(&store, "4821", k_salt_a);
        f.cut_after = 10; /* the power goes while the failure is counted */
        if (ui_pin_verify(&store, "4821") != UI_PIN_UNAVAILABLE) {
            printf("  FAIL: a cut while counting must not read as a success\n");
            failures++;
        }
        f.cut_after = -1;
        if (ui_pin_verify(&store, "4821") != UI_PIN_OK) {
            printf("  FAIL: the old record must be intact after the cut\n");
            failures++;
        }
    }
    return failures;
}

static int flipped_bytes(void)
{
    int failures = 0;
    fram_t good;
    ui_pin_store_t store;
    blank(&good);
    open_store(&store, &good, 1000);
    ui_pin_set(&store, "4821", k_salt_a);
    for (uint32_t index = 0; index < 2u * UI_PIN_RECORD_BYTES; index++) {
        fram_t f = good;
        ui_pin_store_t damaged;
        ui_pin_result_t result;
        f.data[index] ^= 0x5Au;
        open_store(&damaged, &f, 1000);
        result = ui_pin_verify(&damaged, "4821");
        if (index < 66u && result == UI_PIN_OK) {
            printf("  FAIL: a flipped byte %u of the record still let the PIN in\n", (unsigned)index);
            failures++;
        }
        if (index >= 66u && result != UI_PIN_OK) {
            printf("  FAIL: a byte %u outside the record changed the result (%d)\n", (unsigned)index, (int)result);
            failures++;
        }
    }
    return failures;
}

static int sequence_wrap(void)
{
    int failures = 0;
    fram_t f;
    ui_pin_store_t store;
    blank(&f);
    open_store(&store, &f, 1);
    for (uint32_t k = 0; k < 65545u; k++) {
        const char *latest = (k % 2u == 0u) ? "4821" : "9376";
        const char *previous = (k % 2u == 0u) ? "9376" : "4821";
        ui_pin_set(&store, latest, (k % 2u == 0u) ? k_salt_a : k_salt_b);
        if (k >= 65530u) {
            ui_pin_store_t fresh;
            open_store(&fresh, &f, 1);
            if (ui_pin_verify(&fresh, previous) == UI_PIN_OK || ui_pin_verify(&fresh, latest) != UI_PIN_OK) {
                printf("  FAIL: at the wrap of the sequence (set %u) the newer record did not win\n", (unsigned)k);
                failures++;
            }
        }
    }
    return failures;
}

int main(void)
{
    int failures = 0;
    failures += vectors();
    failures += states_and_refusals();
    failures += restart_and_counter();
    failures += torn_writes();
    failures += flipped_bytes();
    failures += sequence_wrap();
    if (failures == 0) {
        printf("ui_pin: PASS\n");
        return 0;
    }
    printf("ui_pin: FAIL (%d)\n", failures);
    return 1;
}
