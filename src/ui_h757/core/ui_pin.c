/* =============================================================================
 * HYDRA-UMC Firmware - src/ui_h757/core/ui_pin.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * The twin of tools/ui_pin.py. Keep the two in the same order of steps: tests/test_ui_pin_c.py compares the bytes of the records and the results of a list of attempts.
 */
#include "ui_pin.h"

#define MAGIC0 'P'
#define MAGIC1 'I'
#define MAGIC2 'N'
#define MAGIC3 '1'
#define VERSION 1u
#define BODY_BYTES 62u /* magic 4, version 1, digits 1, sequence 2, iterations 4, salt 16, hash 32, failures 1, spare 1 */
#define MAX_HASHED_DIGITS 32u

#ifdef UI_PIN_COUNT_BLOCKS
uint32_t ui_sha256_blocks;
#endif

/* ------------------------------------------------------------------ CRC-32 (the one of zlib) */
uint32_t ui_crc32(const uint8_t *data, uint32_t length)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
        }
    }
    return ~crc;
}

/* ------------------------------------------------------------------ SHA-256, HMAC and PBKDF2 */
typedef struct {
    uint32_t h[8];
    uint8_t block[64];
    uint32_t filled;
    uint64_t total;
} sha256_t;

static const uint32_t k_round[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u,
    0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du,
    0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu,
    0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

static uint32_t rotr(uint32_t value, unsigned bits)
{
    return (value >> bits) | (value << (32u - bits));
}

static void sha256_init(sha256_t *s)
{
    static const uint32_t start[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    for (unsigned i = 0; i < 8; i++) {
        s->h[i] = start[i];
    }
    s->filled = 0;
    s->total = 0;
}

static void sha256_compress(sha256_t *s, const uint8_t *chunk)
{
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;
#ifdef UI_PIN_COUNT_BLOCKS
    ui_sha256_blocks++;
#endif
    for (unsigned i = 0; i < 16; i++) {
        w[i] = ((uint32_t)chunk[4 * i] << 24) | ((uint32_t)chunk[4 * i + 1] << 16) | ((uint32_t)chunk[4 * i + 2] << 8) | (uint32_t)chunk[4 * i + 3];
    }
    for (unsigned i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (unsigned i = 0; i < 64; i++) {
        uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k_round[i] + w[i];
        uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha256_update(sha256_t *s, const uint8_t *data, uint32_t length)
{
    s->total += length;
    for (uint32_t i = 0; i < length; i++) {
        s->block[s->filled++] = data[i];
        if (s->filled == 64u) {
            sha256_compress(s, s->block);
            s->filled = 0;
        }
    }
}

static void sha256_final(sha256_t *s, uint8_t out[32])
{
    uint64_t bits = s->total * 8u;
    s->block[s->filled++] = 0x80u;
    if (s->filled > 56u) {
        while (s->filled < 64u) {
            s->block[s->filled++] = 0;
        }
        sha256_compress(s, s->block);
        s->filled = 0;
    }
    while (s->filled < 56u) {
        s->block[s->filled++] = 0;
    }
    for (unsigned i = 0; i < 8; i++) {
        s->block[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
    }
    sha256_compress(s, s->block);
    for (unsigned i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(s->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)s->h[i];
    }
}

/* HMAC-SHA256 with the two padded-key states kept, so that each message of 32 bytes costs two compressions. The key is at most 64 bytes (the PIN is at most 32 digits). */
typedef struct {
    sha256_t inner;
    sha256_t outer;
} hmac_t;

static void hmac_start(hmac_t *m, const uint8_t *key, uint32_t key_length)
{
    uint8_t pad[64];
    for (unsigned i = 0; i < 64; i++) {
        pad[i] = (i < key_length) ? (uint8_t)(key[i] ^ 0x36u) : 0x36u;
    }
    sha256_init(&m->inner);
    sha256_update(&m->inner, pad, 64);
    for (unsigned i = 0; i < 64; i++) {
        pad[i] = (i < key_length) ? (uint8_t)(key[i] ^ 0x5cu) : 0x5cu;
    }
    sha256_init(&m->outer);
    sha256_update(&m->outer, pad, 64);
}

static void hmac_finish(const hmac_t *m, const uint8_t *message, uint32_t length, uint8_t out[32])
{
    sha256_t inner = m->inner;
    sha256_t outer = m->outer;
    uint8_t digest[32];
    sha256_update(&inner, message, length);
    sha256_final(&inner, digest);
    sha256_update(&outer, digest, 32);
    sha256_final(&outer, out);
}

void ui_pbkdf2_sha256(const uint8_t *password, uint32_t password_length, const uint8_t *salt, uint32_t salt_length, uint32_t iterations, uint8_t out[32])
{
    hmac_t keyed;
    uint8_t message[64];
    uint8_t u[32];
    uint8_t t[32];
    uint32_t used = salt_length < 60u ? salt_length : 60u;
    for (uint32_t i = 0; i < used; i++) {
        message[i] = salt[i];
    }
    message[used] = 0;
    message[used + 1] = 0;
    message[used + 2] = 0;
    message[used + 3] = 1;
    hmac_start(&keyed, password, password_length);
    hmac_finish(&keyed, message, used + 4u, u);
    for (unsigned i = 0; i < 32; i++) {
        t[i] = u[i];
    }
    for (uint32_t round = 1; round < iterations; round++) {
        uint8_t next[32];
        hmac_finish(&keyed, u, 32, next);
        for (unsigned i = 0; i < 32; i++) {
            u[i] = next[i];
            t[i] ^= next[i];
        }
    }
    for (unsigned i = 0; i < 32; i++) {
        out[i] = t[i];
    }
}

/* ------------------------------------------------------------------ the records */
typedef struct {
    uint8_t digits;
    uint16_t sequence;
    uint32_t iterations;
    uint8_t salt[UI_PIN_SALT_BYTES];
    uint8_t hash[UI_PIN_HASH_BYTES];
    uint8_t failures;
} record_t;

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void pack(const record_t *r, uint8_t raw[UI_PIN_RECORD_BYTES])
{
    raw[0] = MAGIC0; raw[1] = MAGIC1; raw[2] = MAGIC2; raw[3] = MAGIC3;
    raw[4] = (uint8_t)VERSION;
    raw[5] = r->digits;
    raw[6] = (uint8_t)r->sequence;
    raw[7] = (uint8_t)(r->sequence >> 8);
    put32(raw + 8, r->iterations);
    for (unsigned i = 0; i < UI_PIN_SALT_BYTES; i++) {
        raw[12 + i] = r->salt[i];
    }
    for (unsigned i = 0; i < UI_PIN_HASH_BYTES; i++) {
        raw[28 + i] = r->hash[i];
    }
    raw[60] = r->failures;
    raw[61] = 0;
    put32(raw + BODY_BYTES, ui_crc32(raw, BODY_BYTES));
    for (unsigned i = BODY_BYTES + 4u; i < UI_PIN_RECORD_BYTES; i++) {
        raw[i] = 0xFFu;
    }
}

static int unpack(const uint8_t raw[UI_PIN_RECORD_BYTES], record_t *r)
{
    if (get32(raw + BODY_BYTES) != ui_crc32(raw, BODY_BYTES)) {
        return 0;
    }
    if (raw[0] != MAGIC0 || raw[1] != MAGIC1 || raw[2] != MAGIC2 || raw[3] != MAGIC3 || raw[4] != VERSION) {
        return 0;
    }
    r->digits = raw[5];
    r->sequence = (uint16_t)(raw[6] | (raw[7] << 8));
    r->iterations = get32(raw + 8);
    for (unsigned i = 0; i < UI_PIN_SALT_BYTES; i++) {
        r->salt[i] = raw[12 + i];
    }
    for (unsigned i = 0; i < UI_PIN_HASH_BYTES; i++) {
        r->hash[i] = raw[28 + i];
    }
    r->failures = raw[60];
    return 1;
}

/* The valid record with the newer sequence (the numbers wrap: the newer is the one ahead by less than half the range). Returns its slot, or -1 when there is none. */
static int current(const ui_pin_store_t *store, record_t *out)
{
    int best = -1;
    for (unsigned slot = 0; slot < UI_PIN_SLOTS; slot++) {
        uint8_t raw[UI_PIN_RECORD_BYTES];
        record_t candidate;
        if (store->ports.read(store->ports.context, slot * UI_PIN_RECORD_BYTES, raw, UI_PIN_RECORD_BYTES) != 0 || !unpack(raw, &candidate)) {
            continue;
        }
        if (best < 0) {
            best = (int)slot;
            *out = candidate;
        } else if ((uint16_t)(candidate.sequence - out->sequence) < 0x8000u && candidate.sequence != out->sequence) {
            best = (int)slot;
            *out = candidate;
        }
    }
    return best;
}

static int write_record(const ui_pin_store_t *store, const record_t *r)
{
    record_t old;
    int slot = current(store, &old);
    uint8_t raw[UI_PIN_RECORD_BYTES];
    pack(r, raw);
    return store->ports.write(store->ports.context, (uint32_t)(slot < 0 ? 0 : 1 - slot) * UI_PIN_RECORD_BYTES, raw, UI_PIN_RECORD_BYTES);
}

static uint32_t length_of(const char *text)
{
    uint32_t n = 0;
    while (text[n] != '\0') {
        n++;
    }
    return n;
}

static int all_digits(const char *text, uint32_t length)
{
    if (length == 0) {
        return 0;
    }
    for (uint32_t i = 0; i < length; i++) {
        if (text[i] < '0' || text[i] > '9') {
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ the store */
void ui_pin_init(ui_pin_store_t *store, const ui_pin_ports_t *ports, uint32_t iterations, uint32_t min_digits, uint32_t max_digits)
{
    store->ports = *ports;
    store->iterations = iterations;
    store->min_digits = min_digits;
    store->max_digits = max_digits;
}

ui_pin_state_t ui_pin_state(const ui_pin_store_t *store)
{
    record_t r;
    if (current(store, &r) >= 0) {
        return UI_PIN_STATE_SET;
    }
    {
        int all_ff = 1;
        int all_zero = 1;
        for (unsigned slot = 0; slot < UI_PIN_SLOTS; slot++) {
            uint8_t raw[UI_PIN_RECORD_BYTES];
            if (store->ports.read(store->ports.context, slot * UI_PIN_RECORD_BYTES, raw, UI_PIN_RECORD_BYTES) != 0) {
                return UI_PIN_STATE_CORRUPT;
            }
            for (unsigned i = 0; i < UI_PIN_RECORD_BYTES; i++) {
                all_ff = all_ff && raw[i] == 0xFFu;
                all_zero = all_zero && raw[i] == 0u;
            }
        }
        if (!all_ff && !all_zero) {
            return UI_PIN_STATE_CORRUPT;
        }
    }
    return UI_PIN_STATE_UNSET;
}

uint32_t ui_pin_failures(const ui_pin_store_t *store)
{
    record_t r;
    return current(store, &r) >= 0 ? r.failures : 0u;
}

ui_pin_set_result_t ui_pin_set(ui_pin_store_t *store, const char *pin, const uint8_t salt[UI_PIN_SALT_BYTES])
{
    record_t old;
    record_t r;
    uint32_t length = length_of(pin);
    int has_old = current(store, &old) >= 0;
    int one_digit = 1;
    if (!all_digits(pin, length) || length < store->min_digits || length > store->max_digits) {
        return UI_PIN_SET_BAD_PIN;
    }
    for (uint32_t i = 1; i < length; i++) {
        if (pin[i] != pin[0]) {
            one_digit = 0;
        }
    }
    if (one_digit) {
        return UI_PIN_SET_BAD_PIN;
    }
    if (salt == 0) {
        return UI_PIN_SET_BAD_SALT;
    }
    r.digits = (uint8_t)length;
    r.sequence = has_old ? (uint16_t)(old.sequence + 1u) : 0u;
    r.iterations = store->iterations;
    for (unsigned i = 0; i < UI_PIN_SALT_BYTES; i++) {
        r.salt[i] = salt[i];
    }
    ui_pbkdf2_sha256((const uint8_t *)pin, length, r.salt, UI_PIN_SALT_BYTES, r.iterations, r.hash);
    r.failures = 0;
    return write_record(store, &r) == 0 ? UI_PIN_SET_OK : UI_PIN_SET_WRITE_FAILED;
}

int ui_pin_erase(ui_pin_store_t *store)
{
    uint8_t blank[UI_PIN_RECORD_BYTES];
    int failed = 0;
    for (unsigned i = 0; i < UI_PIN_RECORD_BYTES; i++) {
        blank[i] = 0xFFu;
    }
    for (unsigned slot = 0; slot < UI_PIN_SLOTS; slot++) {
        failed |= store->ports.write(store->ports.context, slot * UI_PIN_RECORD_BYTES, blank, UI_PIN_RECORD_BYTES);
    }
    return failed;
}

ui_pin_result_t ui_pin_verify(const ui_pin_store_t *store, const char *pin)
{
    record_t r;
    record_t counted;
    record_t after;
    uint8_t candidate[UI_PIN_HASH_BYTES];
    uint32_t length = length_of(pin);
    unsigned difference = 0;
    if (current(store, &r) < 0) {
        return UI_PIN_UNAVAILABLE;
    }
    if (!all_digits(pin, length)) {
        return UI_PIN_BAD;
    }
    counted = r;
    counted.sequence = (uint16_t)(r.sequence + 1u);
    counted.failures = r.failures < 255u ? (uint8_t)(r.failures + 1u) : 255u;
    if (write_record(store, &counted) != 0) {
        return UI_PIN_UNAVAILABLE; /* the old record is still there; the attempt does not count as a success */
    }
    if (length > MAX_HASHED_DIGITS) {
        return UI_PIN_BAD; /* no PIN of that length can be set: counted, not hashed */
    }
    ui_pbkdf2_sha256((const uint8_t *)pin, length, r.salt, UI_PIN_SALT_BYTES, r.iterations, candidate);
    for (unsigned i = 0; i < UI_PIN_HASH_BYTES; i++) {
        difference |= (unsigned)(candidate[i] ^ r.hash[i]); /* every byte is looked at: the time does not tell where they differ */
    }
    if (difference != 0u) {
        return UI_PIN_BAD;
    }
    if (current(store, &after) >= 0) {
        record_t cleared = after;
        cleared.sequence = (uint16_t)(after.sequence + 1u);
        cleared.failures = 0;
        (void)write_record(store, &cleared); /* the PIN was right; if this write is cut the counter stays one up, which only costs a try */
    }
    return UI_PIN_OK;
}
