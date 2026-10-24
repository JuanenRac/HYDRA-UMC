/* =============================================================================
 * HYDRA-UMC Firmware - Host logic tests for image_header.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * image_header.h (the signed image header and the anti-rollback rule) exists as a real per-target copy in all THREE bootloaders; the
 * exact same test source is compiled and run once per target's own include path (build_firmware.sh), so a drift between the copies is
 * a test failure. The bytes come from tests/image_header_vectors.h, which tools/gen_image_header_vectors.py makes with the host
 * reference tools/image_header.py: the C code is checked against what the reference produced, not against itself.
 */
#include <stdio.h>
#include <string.h>

#include "image_header.h"
#include "image_header_vectors.h"

static int failures;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("  FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
            failures++; \
        } \
    } while (0)

static ImageHeaderExpect_t good_expectation(void)
{
    ImageHeaderExpect_t e;
    e.hw_id = VEC_HW_ID;
    e.link_address = VEC_LINK_ADDRESS;
    e.image_size = VEC_IMAGE_SIZE;
    e.image_crc32 = VEC_IMAGE_CRC32;
    e.first_word = ImageHeader_Le32(VEC_IMAGE);
    e.second_word = ImageHeader_Le32(VEC_IMAGE + 4);
    e.ram_lo = VEC_RAM_LO;
    e.ram_hi = VEC_RAM_HI;
    return e;
}

static void test_the_reference_header_parses(void)
{
    ImageHeader_t h;
    CHECK(ImageHeader_Parse(VEC_HEADER, &h) == IMAGE_HEADER_OK, "the header made by the reference parses");
    CHECK(h.hw_id == VEC_HW_ID, "hw_id");
    CHECK(h.version.major == VEC_VERSION_MAJOR && h.version.minor == VEC_VERSION_MINOR && h.version.patch == VEC_VERSION_PATCH, "version");
    CHECK(h.image_size == VEC_IMAGE_SIZE, "image_size");
    CHECK(h.link_address == VEC_LINK_ADDRESS, "link_address");
    CHECK(h.vector_sp == VEC_VECTOR_SP && h.vector_reset == VEC_VECTOR_RESET, "vectors");
    CHECK(h.image_crc32 == VEC_IMAGE_CRC32, "image_crc32");
    CHECK(sizeof(VEC_HEADER) == IMAGE_HEADER_SIZE, "the vector header is 64 bytes");
    CHECK(sizeof(VEC_TAG) == IMAGE_TAG_SIZE, "the tag is 32 bytes");
}

static void test_a_header_that_is_not_ours_or_has_reserved_bytes_set_is_refused(void)
{
    uint8_t raw[IMAGE_HEADER_SIZE];
    ImageHeader_t h;
    struct { size_t offset; uint8_t value; ImageHeaderStatus_t expect; const char *what; } cases[] = {
        {0, 0x00, IMAGE_HEADER_BAD_MAGIC, "magic byte 0"},
        {3, 0x00, IMAGE_HEADER_BAD_MAGIC, "magic byte 3"},
        {4, 0x09, IMAGE_HEADER_BAD_FORMAT, "format low byte"},
        {5, 0x01, IMAGE_HEADER_BAD_FORMAT, "format high byte"},
        {6, 0x01, IMAGE_HEADER_RESERVED_NOT_ZERO, "flags low byte"},
        {7, 0x80, IMAGE_HEADER_RESERVED_NOT_ZERO, "flags high byte"},
        {18, 0x01, IMAGE_HEADER_RESERVED_NOT_ZERO, "reserved after the version"},
        {19, 0x01, IMAGE_HEADER_RESERVED_NOT_ZERO, "reserved after the version, high"},
        {40, 0x01, IMAGE_HEADER_RESERVED_NOT_ZERO, "first byte of the padding"},
        {63, 0xFF, IMAGE_HEADER_RESERVED_NOT_ZERO, "last byte of the padding"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        memcpy(raw, VEC_HEADER, sizeof(raw));
        raw[cases[i].offset] = cases[i].value;
        CHECK(ImageHeader_Parse(raw, &h) == cases[i].expect, cases[i].what);
    }
    /* an erased page (all 0xFF) and a page of zeros are no header */
    memset(raw, 0xFF, sizeof(raw));
    CHECK(ImageHeader_Parse(raw, &h) == IMAGE_HEADER_BAD_MAGIC, "erased flash is not a header");
    memset(raw, 0x00, sizeof(raw));
    CHECK(ImageHeader_Parse(raw, &h) == IMAGE_HEADER_BAD_MAGIC, "zeros are not a header");
}

static void test_the_checks_say_why(void)
{
    ImageHeader_t h;
    ImageHeaderExpect_t e = good_expectation();
    ImageHeader_Parse(VEC_HEADER, &h);
    CHECK(ImageHeader_Check(&h, &e) == IMAGE_HEADER_OK, "the reference image passes every check");

    e = good_expectation(); e.image_size = VEC_IMAGE_SIZE + 1;
    CHECK(ImageHeader_Check(&h, &e) == IMAGE_HEADER_SIZE_MISMATCH, "the size received is not the size in the header");
    e = good_expectation(); e.image_crc32 ^= 1u;
    CHECK(ImageHeader_Check(&h, &e) == IMAGE_HEADER_CRC_MISMATCH, "the CRC computed is not the CRC in the header");
    e = good_expectation(); e.hw_id ^= 1u;
    CHECK(ImageHeader_Check(&h, &e) == IMAGE_HEADER_WRONG_HARDWARE, "another board");
    e = good_expectation(); e.link_address += 0x1000u;
    CHECK(ImageHeader_Check(&h, &e) == IMAGE_HEADER_WRONG_LINK_ADDRESS, "linked for another address");
    e = good_expectation(); e.first_word ^= 1u;
    CHECK(ImageHeader_Check(&h, &e) == IMAGE_HEADER_VECTORS_MISMATCH, "the stack word of the image is not the header's");
    e = good_expectation(); e.second_word ^= 2u;
    CHECK(ImageHeader_Check(&h, &e) == IMAGE_HEADER_VECTORS_MISMATCH, "the reset word of the image is not the header's");
    e = good_expectation(); e.ram_hi = VEC_VECTOR_SP - 1u;
    CHECK(ImageHeader_Check(&h, &e) == IMAGE_HEADER_SP_OUTSIDE_RAM, "stack above RAM");
    e = good_expectation(); e.ram_lo = VEC_VECTOR_SP + 1u;
    CHECK(ImageHeader_Check(&h, &e) == IMAGE_HEADER_SP_OUTSIDE_RAM, "stack below RAM");
    e = good_expectation(); e.ram_lo = VEC_VECTOR_SP; e.ram_hi = VEC_VECTOR_SP;
    CHECK(ImageHeader_Check(&h, &e) == IMAGE_HEADER_OK, "the RAM range includes both ends");
}

static void test_the_reset_vector_must_be_thumb_and_inside_the_image(void)
{
    ImageHeader_t h;
    ImageHeaderExpect_t e = good_expectation();
    ImageHeader_Parse(VEC_HEADER, &h);

    ImageHeader_t even = h; even.vector_reset &= ~1u; e.second_word = even.vector_reset;
    CHECK(ImageHeader_Check(&even, &e) == IMAGE_HEADER_RESET_NOT_THUMB, "an even reset vector is not Thumb");

    e = good_expectation();
    ImageHeader_t before = h; before.vector_reset = (VEC_LINK_ADDRESS - 2u) | 1u; e.second_word = before.vector_reset;
    CHECK(ImageHeader_Check(&before, &e) == IMAGE_HEADER_RESET_OUTSIDE_IMAGE, "a reset vector before the image");
    ImageHeader_t last = h; last.vector_reset = (VEC_LINK_ADDRESS + VEC_IMAGE_SIZE - 2u) | 1u; e.second_word = last.vector_reset;
    CHECK(ImageHeader_Check(&last, &e) == IMAGE_HEADER_OK, "the last halfword of the image is inside");
    ImageHeader_t past = h; past.vector_reset = (VEC_LINK_ADDRESS + VEC_IMAGE_SIZE) | 1u; e.second_word = past.vector_reset;
    CHECK(ImageHeader_Check(&past, &e) == IMAGE_HEADER_RESET_OUTSIDE_IMAGE, "the first byte after the image is outside");

    /* nothing wraps around: an image that claims to end past the top of the address space still has to contain its reset vector */
    ImageHeader_t huge = h; huge.image_size = 0xFFFFFFFFu; huge.link_address = 0xFFFFF000u; huge.vector_reset = 0x08009101u;
    ImageHeaderExpect_t big = good_expectation(); big.image_size = 0xFFFFFFFFu; big.link_address = 0xFFFFF000u; big.second_word = huge.vector_reset;
    CHECK(ImageHeader_Check(&huge, &big) == IMAGE_HEADER_RESET_OUTSIDE_IMAGE, "no wrap-around of link + size");
    ImageHeader_t zero = h; zero.image_size = 0; ImageHeaderExpect_t none = good_expectation(); none.image_size = 0;
    CHECK(ImageHeader_Check(&zero, &none) == IMAGE_HEADER_RESET_OUTSIDE_IMAGE, "an empty image contains nothing");
}

static void test_versions_compare_with_the_patch(void)
{
    ImageVersion_t v018 = {0, 1, 8}, v012 = {0, 1, 2}, v020 = {0, 2, 0}, v100 = {1, 0, 0}, v0ffff = {0, 0xFFFFu, 0xFFFFu};
    CHECK(ImageHeader_VersionCompare(&v018, &v018) == 0, "equal");
    CHECK(ImageHeader_VersionCompare(&v012, &v018) == -1, "0.1.2 is older than 0.1.8 (the patch counts)");
    CHECK(ImageHeader_VersionCompare(&v018, &v012) == 1, "0.1.8 is newer than 0.1.2");
    CHECK(ImageHeader_VersionCompare(&v020, &v018) == 1, "minor beats patch");
    CHECK(ImageHeader_VersionCompare(&v100, &v0ffff) == 1, "major beats everything below it");
    CHECK(ImageHeader_VersionCompare(&v0ffff, &v100) == -1, "and the other way");
}

static void test_the_anti_rollback_rule(void)
{
    ImageHeader_t h;
    ImageHeader_Parse(VEC_HEADER, &h); /* 0.2.3 */
    ImageVersion_t older = {0, 2, 2}, same = {0, 2, 3}, newer = {0, 2, 4}, much_newer = {0, 9, 0};
    CHECK(ImageHeader_CheckRollback(&h, NULL, 0) == IMAGE_HEADER_OK, "nothing installed: any image is acceptable");
    CHECK(ImageHeader_CheckRollback(&h, &older, 0) == IMAGE_HEADER_OK, "a newer image passes");
    CHECK(ImageHeader_CheckRollback(&h, &same, 0) == IMAGE_HEADER_NOT_NEWER, "the same version is not newer");
    CHECK(ImageHeader_CheckRollback(&h, &same, 1) == IMAGE_HEADER_NOT_NEWER, "a token never allows the same version");
    CHECK(ImageHeader_CheckRollback(&h, &newer, 0) == IMAGE_HEADER_NOT_NEWER, "an older image is refused");
    CHECK(ImageHeader_CheckRollback(&h, &newer, 1) == IMAGE_HEADER_OK, "an older image passes with an authorized downgrade");
    CHECK(ImageHeader_CheckRollback(&h, &much_newer, 0) == IMAGE_HEADER_NOT_NEWER, "much older is refused too");
}

static void test_the_downgrade_message_is_what_the_reference_signs(void)
{
    ImageVersion_t installed = {VEC_INSTALLED_MAJOR, VEC_INSTALLED_MINOR, VEC_INSTALLED_PATCH};
    ImageVersion_t target = {VEC_B_VERSION_MAJOR, VEC_B_VERSION_MINOR, VEC_B_VERSION_PATCH};
    uint8_t message[DOWNGRADE_MESSAGE_LEN];
    ImageHeader_DowngradeMessage(VEC_NONCE, &installed, &target, VEC_B_TAG, message);
    CHECK(sizeof(VEC_DOWNGRADE_MESSAGE) == DOWNGRADE_MESSAGE_LEN, "the message has the length the reference uses");
    CHECK(memcmp(message, VEC_DOWNGRADE_MESSAGE, DOWNGRADE_MESSAGE_LEN) == 0, "the downgrade message equals the reference's byte for byte");

    /* another pair, another nonce: another message */
    ImageVersion_t other = {VEC_B_VERSION_MAJOR, VEC_B_VERSION_MINOR, VEC_B_VERSION_PATCH + 1u};
    uint8_t second[DOWNGRADE_MESSAGE_LEN];
    ImageHeader_DowngradeMessage(VEC_NONCE, &installed, &other, VEC_B_TAG, second);
    CHECK(memcmp(message, second, DOWNGRADE_MESSAGE_LEN) != 0, "a different target is a different message");
    uint8_t nonce[IMAGE_NONCE_SIZE];
    memcpy(nonce, VEC_NONCE, sizeof(nonce));
    nonce[15] ^= 1u;
    ImageHeader_DowngradeMessage(nonce, &installed, &target, VEC_B_TAG, second);
    CHECK(memcmp(message, second, DOWNGRADE_MESSAGE_LEN) != 0, "a different session is a different message");
    uint8_t other_tag[IMAGE_TAG_SIZE];
    memcpy(other_tag, VEC_B_TAG, sizeof(other_tag));
    other_tag[0] ^= 1u;
    ImageHeader_DowngradeMessage(VEC_NONCE, &installed, &target, other_tag, second);
    CHECK(memcmp(message, second, DOWNGRADE_MESSAGE_LEN) != 0, "another image is another message");
}

int main(void)
{
    printf("HYDRA-UMC host logic tests - image_header.h\n");
    test_the_reference_header_parses();
    test_a_header_that_is_not_ours_or_has_reserved_bytes_set_is_refused();
    test_the_checks_say_why();
    test_the_reset_vector_must_be_thumb_and_inside_the_image();
    test_versions_compare_with_the_patch();
    test_the_anti_rollback_rule();
    test_the_downgrade_message_is_what_the_reference_signs();
    if (failures == 0) {
        printf("  all image_header checks passed\n");
        return 0;
    }
    printf("  %d image_header check(s) FAILED\n", failures);
    return 1;
}
