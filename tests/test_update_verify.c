/* =============================================================================
 * HYDRA-UMC Firmware - Host logic tests for update_verify.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * update_verify.h (the decision at the end of an update: complete, CRC, tag, header, version, anti-rollback with a downgrade token)
 * exists as a real per-target copy in all THREE bootloaders; the exact same test source is compiled and run once per target's own
 * include path (build_firmware.sh). The bytes come from tests/image_header_vectors.h, made by the host reference
 * tools/image_header.py. The HMAC is a test double that answers with the token the reference signed only when it is handed the exact
 * message the reference signed: so the test proves the C code builds that message byte for byte, without an HMAC of its own (the real
 * HMAC against the reference is tested in test_bootloader_g474_host.c).
 */
#include <stdio.h>
#include <string.h>

#include "update_verify.h"
#include "image_header_vectors.h"

static int failures;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("  FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
            failures++; \
        } \
    } while (0)

static int hmac_calls;

static void reference_hmac(const uint8_t *msg, uint32_t len, uint8_t out[32], void *context)
{
    (void)context;
    hmac_calls++;
    if (len == sizeof(VEC_DOWNGRADE_MESSAGE) && memcmp(msg, VEC_DOWNGRADE_MESSAGE, len) == 0) {
        memcpy(out, VEC_DOWNGRADE_TOKEN, 32);
        return;
    }
    for (int i = 0; i < 32; i++) out[i] = (uint8_t)(0xA5 ^ (len + (uint32_t)i)); /* anything that is not the signed token */
}

static uint8_t header_buffer[IMAGE_HEADER_SIZE];
static uint8_t tag_expected[IMAGE_TAG_SIZE];
static uint8_t tag_actual[IMAGE_TAG_SIZE];
static uint8_t token[IMAGE_TAG_SIZE];
static uint8_t nonce[IMAGE_NONCE_SIZE];

/* Image A (0.2.3) over an installed 0.2.2: everything as it should arrive. */
static UpdateVerifyInput_t good_a(void)
{
    UpdateVerifyInput_t in;
    memset(&in, 0, sizeof(in));
    memcpy(header_buffer, VEC_HEADER, sizeof(header_buffer));
    memcpy(tag_expected, VEC_TAG, sizeof(tag_expected));
    memcpy(tag_actual, VEC_TAG, sizeof(tag_actual));
    memset(token, 0, sizeof(token));
    memcpy(nonce, VEC_NONCE, sizeof(nonce));
    in.bytes_received = VEC_IMAGE_SIZE;
    in.total_size = VEC_IMAGE_SIZE;
    in.hmac_chunks = UPDATE_HMAC_CHUNKS;
    in.header_chunks = UPDATE_HEADER_CHUNKS;
    in.token_chunks = 0;
    in.declared_crc = VEC_IMAGE_CRC32;
    in.actual_crc = VEC_IMAGE_CRC32;
    in.declared_major = VEC_VERSION_MAJOR;
    in.declared_minor = VEC_VERSION_MINOR;
    in.header = header_buffer;
    in.expected_tag = tag_expected;
    in.actual_tag = tag_actual;
    in.token = token;
    in.nonce = nonce;
    in.expect.hw_id = VEC_HW_ID;
    in.expect.link_address = VEC_LINK_ADDRESS;
    in.expect.first_word = ImageHeader_Le32(VEC_IMAGE);
    in.expect.second_word = ImageHeader_Le32(VEC_IMAGE + 4);
    in.expect.ram_lo = VEC_RAM_LO;
    in.expect.ram_hi = VEC_RAM_HI;
    in.have_installed = 1;
    in.installed.major = VEC_INSTALLED_MAJOR;
    in.installed.minor = VEC_INSTALLED_MINOR;
    in.installed.patch = VEC_INSTALLED_PATCH;
    in.hmac = reference_hmac;
    return in;
}

/* Image B (0.1.8), older than the installed 0.2.2; the token chunks are complete when `with_token`. */
static UpdateVerifyInput_t good_b(int with_token)
{
    UpdateVerifyInput_t in = good_a();
    memcpy(header_buffer, VEC_B_HEADER, sizeof(header_buffer));
    memcpy(tag_expected, VEC_B_TAG, sizeof(tag_expected));
    memcpy(tag_actual, VEC_B_TAG, sizeof(tag_actual));
    in.declared_crc = VEC_B_IMAGE_CRC32;
    in.actual_crc = VEC_B_IMAGE_CRC32;
    in.declared_major = VEC_B_VERSION_MAJOR;
    in.declared_minor = VEC_B_VERSION_MINOR;
    in.expect.first_word = ImageHeader_Le32(VEC_B_IMAGE);
    in.expect.second_word = ImageHeader_Le32(VEC_B_IMAGE + 4);
    if (with_token) {
        memcpy(token, VEC_DOWNGRADE_TOKEN, sizeof(token));
        in.token_chunks = UPDATE_TOKEN_CHUNKS;
    }
    return in;
}

static void test_a_good_update_is_accepted_and_says_what_it_installs(void)
{
    UpdateVerifyInput_t in = good_a();
    ImageHeader_t h;
    memset(&h, 0, sizeof(h));
    CHECK(UpdateVerify_Decide(&in, &h) == UPDATE_VERDICT_OK, "the reference image over 0.2.2");
    CHECK(h.version.major == VEC_VERSION_MAJOR && h.version.minor == VEC_VERSION_MINOR && h.version.patch == VEC_VERSION_PATCH, "the version comes from the header");
    in.have_installed = 0;
    CHECK(UpdateVerify_Decide(&in, NULL) == UPDATE_VERDICT_OK, "with nothing installed (and no output wanted)");
    CHECK(hmac_calls == 0, "no downgrade token was computed for a plain update");
}

static void test_an_update_that_did_not_arrive_whole_is_incomplete(void)
{
    UpdateVerifyInput_t in = good_a();
    in.bytes_received--;
    CHECK(UpdateVerify_Decide(&in, NULL) == UPDATE_VERDICT_INCOMPLETE, "a byte missing");
    in = good_a(); in.hmac_chunks = 3;
    CHECK(UpdateVerify_Decide(&in, NULL) == UPDATE_VERDICT_INCOMPLETE, "three tag chunks");
    in = good_a(); in.hmac_chunks = 5;
    CHECK(UpdateVerify_Decide(&in, NULL) == UPDATE_VERDICT_INCOMPLETE, "five tag chunks");
    for (uint8_t chunks = 0; chunks <= 9; chunks++) {
        in = good_a(); in.header_chunks = chunks;
        CHECK((UpdateVerify_Decide(&in, NULL) == UPDATE_VERDICT_OK) == (chunks == UPDATE_HEADER_CHUNKS), "only eight header chunks are a header");
    }
}

static void test_the_order_of_the_checks(void)
{
    UpdateVerifyInput_t in = good_a();
    in.actual_crc ^= 1u;
    CHECK(UpdateVerify_Decide(&in, NULL) == UPDATE_VERDICT_CRC32, "CRC");

    in = good_a(); tag_actual[31] ^= 1u;
    CHECK(UpdateVerify_Decide(&in, NULL) == UPDATE_VERDICT_HMAC, "a tag that does not match");
    in = good_a(); tag_expected[0] ^= 0x80u;
    CHECK(UpdateVerify_Decide(&in, NULL) == UPDATE_VERDICT_HMAC, "an expected tag that was changed");

    /* nothing in the header is believed before the tag holds: a header that is not a header AND a wrong tag is a tag failure */
    in = good_a(); header_buffer[0] = 0; tag_actual[5] ^= 1u;
    CHECK(UpdateVerify_Decide(&in, NULL) == UPDATE_VERDICT_HMAC, "the tag is judged before the header");
    in = good_a(); header_buffer[0] = 0;
    CHECK(UpdateVerify_Decide(&in, NULL) == UPDATE_VERDICT_HEADER, "with the tag right, a bad magic is a header failure");
}

static void test_the_header_must_describe_what_arrived(void)
{
    UpdateVerifyInput_t in = good_a();
    in.expect.hw_id ^= 1u;
    CHECK(UpdateVerify_Decide(&in, NULL) == UPDATE_VERDICT_HARDWARE_ID, "another board");
    in = good_a(); in.expect.link_address += 0x800u;
    CHECK(UpdateVerify_Decide(&in, NULL) == UPDATE_VERDICT_HEADER, "linked for another address");
    in = good_a(); in.expect.first_word ^= 4u;
    CHECK(UpdateVerify_Decide(&in, NULL) == UPDATE_VERDICT_HEADER, "the first word of the image is not the header's stack");
    in = good_a(); in.expect.ram_hi = VEC_RAM_LO;
    CHECK(UpdateVerify_Decide(&in, NULL) == UPDATE_VERDICT_HEADER, "stack outside RAM");
    in = good_a(); in.total_size = VEC_IMAGE_SIZE; in.bytes_received = VEC_IMAGE_SIZE; header_buffer[20] ^= 1u;
    CHECK(UpdateVerify_Decide(&in, NULL) == UPDATE_VERDICT_HEADER, "a header that declares another size");
    in = good_a(); in.declared_minor++;
    CHECK(UpdateVerify_Decide(&in, NULL) == UPDATE_VERDICT_HEADER, "END_UPDATE announces another minor than the header");
    in = good_a(); in.declared_major++;
    CHECK(UpdateVerify_Decide(&in, NULL) == UPDATE_VERDICT_HEADER, "END_UPDATE announces another major than the header");
}

static void test_an_image_must_be_strictly_newer(void)
{
    UpdateVerifyInput_t in = good_a();
    in.installed.patch = VEC_VERSION_PATCH; /* 0.2.3 over 0.2.3 */
    CHECK(UpdateVerify_Decide(&in, NULL) == UPDATE_VERDICT_ROLLBACK, "the same version");
    in = good_a(); in.installed.patch = VEC_VERSION_PATCH + 1u;
    CHECK(UpdateVerify_Decide(&in, NULL) == UPDATE_VERDICT_ROLLBACK, "one patch older");
    in = good_a(); in.installed.minor = VEC_VERSION_MINOR + 1u; in.installed.patch = 0;
    CHECK(UpdateVerify_Decide(&in, NULL) == UPDATE_VERDICT_ROLLBACK, "a lower patch under a higher minor is older");
    in = good_a(); in.installed.major = 0; in.installed.minor = 2; in.installed.patch = 0;
    CHECK(UpdateVerify_Decide(&in, NULL) == UPDATE_VERDICT_OK, "a higher patch is newer");
    /* the old flow compared only major.minor: 0.2.3 over 0.2.9 was not noticed */
    in = good_a(); in.installed.patch = 9;
    CHECK(UpdateVerify_Decide(&in, NULL) == UPDATE_VERDICT_ROLLBACK, "the patch counts: 0.2.3 is older than 0.2.9");
}

static void test_a_downgrade_needs_the_token_made_for_this_image_this_pair_and_this_session(void)
{
    UpdateVerifyInput_t in = good_b(0);
    CHECK(UpdateVerify_Decide(&in, NULL) == UPDATE_VERDICT_ROLLBACK, "image B is older and has no token");

    hmac_calls = 0;
    in = good_b(1);
    CHECK(UpdateVerify_Decide(&in, NULL) == UPDATE_VERDICT_OK, "image B with the token the reference signed for 0.2.2 -> 0.1.8");
    CHECK(hmac_calls == 1, "the token was computed once");

    in = good_b(1); token[7] ^= 1u;
    CHECK(UpdateVerify_Decide(&in, NULL) == UPDATE_VERDICT_ROLLBACK, "a token with one bit changed");
    in = good_b(1); in.token_chunks = UPDATE_TOKEN_CHUNKS - 1u;
    CHECK(UpdateVerify_Decide(&in, NULL) == UPDATE_VERDICT_ROLLBACK, "three of four token chunks");
    in = good_b(1); nonce[0] ^= 1u;
    CHECK(UpdateVerify_Decide(&in, NULL) == UPDATE_VERDICT_ROLLBACK, "another session (another nonce)");
    in = good_b(1); in.installed.patch++;
    CHECK(UpdateVerify_Decide(&in, NULL) == UPDATE_VERDICT_ROLLBACK, "another installed version");
    in = good_b(1); in.hmac = NULL;
    CHECK(UpdateVerify_Decide(&in, NULL) == UPDATE_VERDICT_ROLLBACK, "without an HMAC there is no downgrade");

    /* the token of image B does not authorize image A's tag, nor does any token allow the same version */
    in = good_a(); memcpy(token, VEC_DOWNGRADE_TOKEN, sizeof(token)); in.token_chunks = UPDATE_TOKEN_CHUNKS;
    in.installed.patch = VEC_VERSION_PATCH;
    CHECK(UpdateVerify_Decide(&in, NULL) == UPDATE_VERDICT_ROLLBACK, "a token never lets the same version in");
    in = good_a(); memcpy(token, VEC_DOWNGRADE_TOKEN, sizeof(token)); in.token_chunks = UPDATE_TOKEN_CHUNKS;
    in.installed.patch = VEC_VERSION_PATCH + 1u;
    CHECK(UpdateVerify_Decide(&in, NULL) == UPDATE_VERDICT_ROLLBACK, "the token of image B does not downgrade to image A");
}

static void test_a_token_is_not_computed_when_there_is_nothing_to_downgrade_from(void)
{
    hmac_calls = 0;
    UpdateVerifyInput_t in = good_b(1);
    in.have_installed = 0;
    CHECK(UpdateVerify_Decide(&in, NULL) == UPDATE_VERDICT_OK, "with nothing installed any valid image is accepted");
    CHECK(hmac_calls == 0, "and no token is looked at");
}

int main(void)
{
    printf("HYDRA-UMC host logic tests - update_verify.h\n");
    test_a_good_update_is_accepted_and_says_what_it_installs();
    test_an_update_that_did_not_arrive_whole_is_incomplete();
    test_the_order_of_the_checks();
    test_the_header_must_describe_what_arrived();
    test_an_image_must_be_strictly_newer();
    test_a_downgrade_needs_the_token_made_for_this_image_this_pair_and_this_session();
    test_a_token_is_not_computed_when_there_is_nothing_to_downgrade_from();
    if (failures == 0) {
        printf("  all update_verify checks passed\n");
        return 0;
    }
    printf("  %d update_verify check(s) FAILED\n", failures);
    return 1;
}
