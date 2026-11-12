/* =============================================================================
 * HYDRA-UMC Firmware - host logic tests for the format 2 header and its update decision (image_header.h, update_verify.h)
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * The three bootloaders carry the same header parser and the same decision with a trust table. This test builds its headers by hand and answers the
 * signature check with a double that accepts a marked signature, so every refusal of ImageHeader_Parse2 and every verdict of UpdateVerify_Decide2 is
 * reached on each of the three, in the order of the decision, and a sweep with a fixed seed alters the header byte by byte. The shared vectors of the
 * host reference are run by the bootloader tests; the cryptography itself by test_bootloader_trust.c. Evidence: simulator.
 */
#include <stdio.h>
#include <string.h>

#include "update_verify.h"

static int failures;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("  FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
            failures++; \
        } \
    } while (0)

#define HW_ID     0x47343130u
#define LINK      0x08009000u
#define SP        0x20010000u
#define RESET     0x08009041u
#define IMG_SIZE  0x60u
#define IMG_CRC   0xAB4090EFu
#define RAM_LO    0x20000000u
#define RAM_HI    0x20020000u

static const uint8_t KEY_IDS[2][IMAGE_KEY_ID_SIZE] = {{1, 2, 3, 4, 5, 6, 7, 8}, {9, 9, 9, 9, 9, 9, 9, 9}};
static uint8_t KEYS[2][32];

static int image_checks, token_checks;
static const uint8_t *last_key;

/* The double: an image signature is good when its first byte is 0x55, a downgrade token when its first byte is 0x66. */
static int fake_verify(const uint8_t *msg, uint32_t len, const uint8_t signature[IMAGE_SIGNATURE_SIZE], const uint8_t public_key[32], void *context)
{
    (void)msg;
    (void)context;
    last_key = public_key;
    if (len == IMAGE_MESSAGE_2_LEN) {
        image_checks++;
        return signature[0] == 0x55u;
    }
    token_checks++;
    return len == DOWNGRADE_MESSAGE_LEN && signature[0] == 0x66u;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void make_header(uint8_t raw[IMAGE_HEADER_SIZE], uint16_t major, uint16_t minor, uint16_t patch, uint32_t counter, int key)
{
    memset(raw, 0, IMAGE_HEADER_SIZE);
    put32(raw, IMAGE_HEADER_MAGIC);
    ImageHeader_PutLe16(raw + 4, IMAGE_HEADER_FORMAT_2);
    put32(raw + 8, HW_ID);
    ImageHeader_PutLe16(raw + 12, major);
    ImageHeader_PutLe16(raw + 14, minor);
    ImageHeader_PutLe16(raw + 16, patch);
    put32(raw + 20, IMG_SIZE);
    put32(raw + 24, LINK);
    put32(raw + 28, SP);
    put32(raw + 32, RESET);
    put32(raw + 36, IMG_CRC);
    put32(raw + 40, counter);
    memcpy(raw + 44, KEY_IDS[key], IMAGE_KEY_ID_SIZE);
}

static uint8_t header[IMAGE_HEADER_SIZE];
static uint8_t signature[IMAGE_SIGNATURE_SIZE];
static uint8_t token[IMAGE_SIGNATURE_SIZE];
static uint8_t digest[IMAGE_DIGEST_SIZE];
static uint8_t nonce[IMAGE_NONCE_SIZE];

/* A complete update of 1.4.0 over an installed 1.3.0, signed, with the rollback counter 5 over a floor of 3. */
static UpdateVerify2Input_t good(void)
{
    UpdateVerify2Input_t in;
    memset(&in, 0, sizeof(in));
    make_header(header, 1, 4, 0, 5, 0);
    memset(signature, 0, sizeof(signature));
    signature[0] = 0x55u;
    memset(token, 0, sizeof(token));
    memset(digest, 0xD1, sizeof(digest));
    memset(nonce, 0x4E, sizeof(nonce));
    in.bytes_received = IMG_SIZE;
    in.total_size = IMG_SIZE;
    in.signature_chunks = UPDATE_SIGNATURE_CHUNKS;
    in.header_chunks = UPDATE_HEADER_CHUNKS;
    in.declared_crc = IMG_CRC;
    in.actual_crc = IMG_CRC;
    in.declared_major = 1;
    in.declared_minor = 4;
    in.header = header;
    in.signature = signature;
    in.actual_digest = digest;
    in.token = token;
    in.nonce = nonce;
    in.expect.hw_id = HW_ID;
    in.expect.link_address = LINK;
    in.expect.first_word = SP;
    in.expect.second_word = RESET;
    in.expect.ram_lo = RAM_LO;
    in.expect.ram_hi = RAM_HI;
    in.have_installed = 1;
    in.installed.major = 1;
    in.installed.minor = 3;
    in.counter_floor = 3;
    in.key_count = 2;
    in.key_ids = KEY_IDS;
    in.keys = (const uint8_t (*)[32])KEYS;
    in.verify = fake_verify;
    return in;
}

static void test_the_parser_reads_what_it_should_and_refuses_the_rest(void)
{
    ImageHeader2_t out;
    make_header(header, 2, 7, 9, 0xCAFE, 1);
    CHECK(ImageHeader_Parse2(header, &out) == IMAGE_HEADER_OK, "a well-formed header is read");
    CHECK(out.base.hw_id == HW_ID && out.base.version.major == 2 && out.base.version.minor == 7 && out.base.version.patch == 9, "hardware and version");
    CHECK(out.base.image_size == IMG_SIZE && out.base.link_address == LINK && out.base.vector_sp == SP && out.base.vector_reset == RESET, "size, link and vectors");
    CHECK(out.base.image_crc32 == IMG_CRC && out.rollback_counter == 0xCAFEu, "crc and rollback counter");
    CHECK(memcmp(out.key_id, KEY_IDS[1], IMAGE_KEY_ID_SIZE) == 0, "key id");

    make_header(header, 1, 0, 0, 0, 0);
    header[0] ^= 1u;
    CHECK(ImageHeader_Parse2(header, &out) == IMAGE_HEADER_BAD_MAGIC, "a wrong magic");
    make_header(header, 1, 0, 0, 0, 0);
    header[4] = 1u;
    CHECK(ImageHeader_Parse2(header, &out) == IMAGE_HEADER_BAD_FORMAT, "format 1 is not format 2");
    header[4] = 3u;
    CHECK(ImageHeader_Parse2(header, &out) == IMAGE_HEADER_BAD_FORMAT, "format 3 is not format 2");
    make_header(header, 1, 0, 0, 0, 0);
    header[6] = 1u;
    CHECK(ImageHeader_Parse2(header, &out) == IMAGE_HEADER_RESERVED_NOT_ZERO, "flags set");
    make_header(header, 1, 0, 0, 0, 0);
    header[19] = 0x80u;
    CHECK(ImageHeader_Parse2(header, &out) == IMAGE_HEADER_RESERVED_NOT_ZERO, "the reserved half-word at 18 set");
    for (size_t i = 52; i < IMAGE_HEADER_SIZE; i++) {
        make_header(header, 1, 0, 0, 0, 0);
        header[i] = 1u;
        CHECK(ImageHeader_Parse2(header, &out) == IMAGE_HEADER_RESERVED_NOT_ZERO, "a byte of the reserved tail set");
    }
}

/* A fixed-seed sweep: every byte of a good header, flipped in turn by a pseudo-random mask; the bytes the parser pins refuse, the rest read. */
static void test_a_sweep_over_the_header_bytes(void)
{
    uint32_t state = 0x20260101u;
    for (size_t i = 0; i < IMAGE_HEADER_SIZE; i++) {
        state = state * 1664525u + 1013904223u;
        const uint8_t mask = (uint8_t)((state >> 24) | 1u);
        ImageHeader2_t out;
        make_header(header, 1, 4, 0, 5, 0);
        header[i] ^= mask;
        const ImageHeaderStatus_t status = ImageHeader_Parse2(header, &out);
        const int pinned = i < 8u || i == 18u || i == 19u || i >= 52u;
        if (pinned) CHECK(status != IMAGE_HEADER_OK, "a flipped byte of a pinned field is refused");
        else CHECK(status == IMAGE_HEADER_OK, "a flipped byte of a free field is read (the decision judges it later)");
    }
}

static void test_the_signature_message_has_the_domain_and_the_digest(void)
{
    uint8_t message[IMAGE_MESSAGE_2_LEN];
    uint8_t d[IMAGE_DIGEST_SIZE];
    for (size_t i = 0; i < sizeof(d); i++) d[i] = (uint8_t)(i * 7u + 1u);
    ImageHeader_SignatureMessage2(d, message);
    CHECK(memcmp(message, "hydra-umc-image-sig/2", IMAGE_DOMAIN_2_LEN - 1u) == 0 && message[IMAGE_DOMAIN_2_LEN - 1u] == 0u, "the domain of format 2, zero ended");
    CHECK(memcmp(message + IMAGE_DOMAIN_2_LEN, d, sizeof(d)) == 0, "then the digest");
    uint8_t one[IMAGE_MESSAGE_LEN];
    ImageHeader_SignatureMessage(d, one);
    CHECK(memcmp(one, message, IMAGE_DOMAIN_LEN) != 0, "the domain differs from format 1's: a signature of one is not good for the other");
}

static void test_each_verdict_of_the_decision_in_its_order(void)
{
    UpdateVerify2Input_t in = good();
    ImageHeader2_t out;
    memset(&out, 0, sizeof(out));
    image_checks = token_checks = 0;
    CHECK(UpdateVerify_Decide2(&in, &out) == UPDATE_VERDICT_OK, "a good update is accepted");
    CHECK(out.rollback_counter == 5u && out.base.version.minor == 4, "and says what it installs");
    CHECK(last_key == KEYS[0], "with the key the id names");
    CHECK(image_checks == 1 && token_checks == 0, "one signature check, no token for a newer version");
    CHECK(UpdateVerify_Decide2(&in, NULL) == UPDATE_VERDICT_OK, "no output wanted");

    in = good();
    in.bytes_received = IMG_SIZE - 1u;
    CHECK(UpdateVerify_Decide2(&in, NULL) == UPDATE_VERDICT_INCOMPLETE, "missing image bytes");
    in = good();
    in.signature_chunks = UPDATE_SIGNATURE_CHUNKS - 1u;
    CHECK(UpdateVerify_Decide2(&in, NULL) == UPDATE_VERDICT_INCOMPLETE, "missing signature chunk");
    in = good();
    in.header_chunks = 0;
    CHECK(UpdateVerify_Decide2(&in, NULL) == UPDATE_VERDICT_INCOMPLETE, "missing header chunks");
    in = good();
    in.declared_crc ^= 1u;
    CHECK(UpdateVerify_Decide2(&in, NULL) == UPDATE_VERDICT_CRC32, "CRC of END_UPDATE differs");

    in = good();
    header[0] ^= 1u;
    CHECK(UpdateVerify_Decide2(&in, NULL) == UPDATE_VERDICT_HEADER, "a header that is not ours");

    in = good();
    in.key_count = 0;
    CHECK(UpdateVerify_Decide2(&in, NULL) == UPDATE_VERDICT_UNKNOWN_KEY, "an empty trust table");
    in = good();
    make_header(header, 1, 4, 0, 5, 0);
    header[44] ^= 0xFFu;
    CHECK(UpdateVerify_Decide2(&in, NULL) == UPDATE_VERDICT_UNKNOWN_KEY, "a key id that names nothing");

    in = good();
    make_header(header, 1, 4, 0, 5, 1);
    image_checks = 0;
    CHECK(UpdateVerify_Decide2(&in, NULL) == UPDATE_VERDICT_OK && last_key == KEYS[1], "the second key of the table is the one that verifies");
    in = good();
    in.key_count = 1;
    make_header(header, 1, 4, 0, 5, 1);
    CHECK(UpdateVerify_Decide2(&in, NULL) == UPDATE_VERDICT_UNKNOWN_KEY, "a key beyond the count is not trusted");

    in = good();
    signature[0] = 0x00u;
    CHECK(UpdateVerify_Decide2(&in, NULL) == UPDATE_VERDICT_SIGNATURE, "a bad signature");
    in = good();
    in.verify = NULL;
    CHECK(UpdateVerify_Decide2(&in, NULL) == UPDATE_VERDICT_SIGNATURE, "no verifier means no trust");

    in = good();
    in.expect.hw_id ^= 1u;
    CHECK(UpdateVerify_Decide2(&in, NULL) == UPDATE_VERDICT_HARDWARE_ID, "another hardware id");
    in = good();
    in.total_size = in.bytes_received = IMG_SIZE + 4u;
    CHECK(UpdateVerify_Decide2(&in, NULL) == UPDATE_VERDICT_HEADER, "a size the header does not announce");
    in = good();
    in.expect.link_address ^= 0x1000u;
    CHECK(UpdateVerify_Decide2(&in, NULL) == UPDATE_VERDICT_HEADER, "another link address");
    in = good();
    in.expect.first_word ^= 4u;
    CHECK(UpdateVerify_Decide2(&in, NULL) == UPDATE_VERDICT_HEADER, "vectors that are not the image's");
    in = good();
    in.declared_major = 2;
    CHECK(UpdateVerify_Decide2(&in, NULL) == UPDATE_VERDICT_HEADER, "END_UPDATE announces another major");
    in = good();
    in.declared_minor = 5;
    CHECK(UpdateVerify_Decide2(&in, NULL) == UPDATE_VERDICT_HEADER, "END_UPDATE announces another minor");

    in = good();
    in.counter_floor = 6;
    CHECK(UpdateVerify_Decide2(&in, NULL) == UPDATE_VERDICT_COUNTER, "a counter below the floor");
    in.counter_floor = 5;
    CHECK(UpdateVerify_Decide2(&in, NULL) == UPDATE_VERDICT_OK, "a counter equal to the floor passes");
}

static void test_downgrades_need_a_token_of_this_session_and_never_pass_the_counter(void)
{
    UpdateVerify2Input_t in = good();
    in.installed.major = 1;
    in.installed.minor = 6;                    /* the update (1.4.0) is older than 1.6.0 */
    CHECK(UpdateVerify_Decide2(&in, NULL) == UPDATE_VERDICT_ROLLBACK, "an older image without a token");
    in.token_chunks = UPDATE_TOKEN_CHUNKS - 1u;
    token[0] = 0x66u;
    token_checks = 0;
    CHECK(UpdateVerify_Decide2(&in, NULL) == UPDATE_VERDICT_ROLLBACK && token_checks == 0, "an incomplete token is not even read");
    in.token_chunks = UPDATE_TOKEN_CHUNKS;
    token[0] = 0x00u;
    CHECK(UpdateVerify_Decide2(&in, NULL) == UPDATE_VERDICT_ROLLBACK && token_checks == 1, "a token that does not verify");
    token[0] = 0x66u;
    CHECK(UpdateVerify_Decide2(&in, NULL) == UPDATE_VERDICT_OK, "a token that verifies authorises the downgrade");
    in.counter_floor = 6;
    CHECK(UpdateVerify_Decide2(&in, NULL) == UPDATE_VERDICT_COUNTER, "but the rollback counter is never overridable");
    in.counter_floor = 3;
    in.installed.minor = 4;                    /* the same version never passes, token or not */
    CHECK(UpdateVerify_Decide2(&in, NULL) == UPDATE_VERDICT_ROLLBACK, "the same version is not newer");
    in.have_installed = 0;
    CHECK(UpdateVerify_Decide2(&in, NULL) == UPDATE_VERDICT_OK, "with nothing installed there is nothing to roll back");
}

int main(void)
{
    printf("HYDRA-UMC host logic tests - the format 2 header and its update decision\n");
    for (size_t k = 0; k < 2; k++)
        for (size_t i = 0; i < 32; i++) KEYS[k][i] = (uint8_t)(k * 16u + i);
    test_the_parser_reads_what_it_should_and_refuses_the_rest();
    test_a_sweep_over_the_header_bytes();
    test_the_signature_message_has_the_domain_and_the_digest();
    test_each_verdict_of_the_decision_in_its_order();
    test_downgrades_need_a_token_of_this_session_and_never_pass_the_counter();
    if (failures) printf("%d FAILED\n", failures);
    else printf("ok\n");
    return failures ? 1 : 0;
}
