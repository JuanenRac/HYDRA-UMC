// =============================================================================
// HYDRA-UMC Bootloader - Signed firmware image header and anti-rollback rule
// Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
// GPL-3.0-or-later - see repo root LICENSE
// =============================================================================
// Pure logic, no HAL and no flash access, so it runs unchanged on the host (tests/test_image_header.c) and in the bootloader. The same
// file lives in the three bootloaders (src/mcu_stm32g474/boot, src/mcu_stm32h745/CM4/boot, src/mcu_stm32h745/CM7/boot); tests/
// test_image_header.c is compiled against each of them, and tools/image_header.py is the host reference whose byte layout and
// decisions this file reproduces (tests/image_header_vectors.h is generated from it).
//
// Why it exists: an update used to carry the version of the new image in END_UPDATE, outside the data the HMAC covers, and compared
// only major.minor with a constant magic frame to allow a downgrade. So an old signed image could be replayed declaring a higher
// version, and 0.1.8 -> 0.1.2 was not noticed. Here the version (major, minor, patch), the hardware id, the link address, the size, the
// vector words and the CRC are inside a fixed 64-byte header, and an Ed25519 signature covers header || image together (through the
// SHA-256 digest of both): none of them can be changed without breaking the signature. "A new image must be strictly newer than the
// installed one" compares the three numbers.
//
// Header layout, little endian, 64 bytes, then the image, then the 64-byte signature:
//
//   offset size field
//      0    4   magic          0x52445948 ("HYDR": bytes 48 59 44 52)
//      4    2   format         1
//      6    2   flags          0 (must be zero)
//      8    4   hw_id          the 32-bit hardware id of the bootloader (THIS_HARDWARE_ID)
//     12    2   major
//     14    2   minor
//     16    2   patch
//     18    2   reserved       0
//     20    4   image_size     bytes of image after the header
//     24    4   link_address   where the image is linked to run (MAIN_APP_ADDR)
//     28    4   vector_sp      first word of the image (initial stack pointer)
//     32    4   vector_reset   second word of the image (reset vector, odd: Thumb)
//     36    4   image_crc32    CRC-32 of the image bytes
//     40   24   reserved       zeros
//
// The signature is over IMAGE_DOMAIN || SHA-256(header || image) (ImageHeader_SignatureMessage). The caller hashes the image out of the
// flash and checks the signature FIRST: nothing in the header is believed before it verifies.
// The functions below decide everything else, in the order the host reference does.
#ifndef IMAGE_HEADER_H
#define IMAGE_HEADER_H

#include <stddef.h>
#include <stdint.h>

#define IMAGE_HEADER_SIZE   64u
#define IMAGE_HEADER_MAGIC  0x52445948UL
#define IMAGE_HEADER_FORMAT 1u
#define IMAGE_DIGEST_SIZE    32u   // SHA-256 over header || image
#define IMAGE_SIGNATURE_SIZE 64u   // Ed25519
#define IMAGE_NONCE_SIZE     16u
// "hydra-umc-image-sig" and its terminating zero: the start of the message an image signature is made over
#define IMAGE_DOMAIN_LEN     20u
// the message: domain (20) || the 32-byte digest
#define IMAGE_MESSAGE_LEN    (IMAGE_DOMAIN_LEN + IMAGE_DIGEST_SIZE)
// "hydra-umc-downgrade" and its terminating zero: the start of the message a downgrade token is the signature of
#define DOWNGRADE_TAG_LEN    20u
// the message: tag (20) || nonce (16) || installed major, minor, patch (3 x u16 LE) || target major, minor, patch (3 x u16 LE) ||
// the 32-byte digest of the image to be installed
#define DOWNGRADE_MESSAGE_LEN (DOWNGRADE_TAG_LEN + IMAGE_NONCE_SIZE + 12u + IMAGE_DIGEST_SIZE)

typedef struct {
    uint16_t major;
    uint16_t minor;
    uint16_t patch;
} ImageVersion_t;

typedef struct {
    uint32_t       hw_id;
    ImageVersion_t version;
    uint32_t       image_size;
    uint32_t       link_address;
    uint32_t       vector_sp;
    uint32_t       vector_reset;
    uint32_t       image_crc32;
} ImageHeader_t;

typedef enum {
    IMAGE_HEADER_OK = 0,
    IMAGE_HEADER_BAD_MAGIC,
    IMAGE_HEADER_BAD_FORMAT,
    IMAGE_HEADER_RESERVED_NOT_ZERO,
    IMAGE_HEADER_SIZE_MISMATCH,
    IMAGE_HEADER_CRC_MISMATCH,
    IMAGE_HEADER_WRONG_HARDWARE,
    IMAGE_HEADER_WRONG_LINK_ADDRESS,
    IMAGE_HEADER_VECTORS_MISMATCH,
    IMAGE_HEADER_RESET_NOT_THUMB,
    IMAGE_HEADER_RESET_OUTSIDE_IMAGE,
    IMAGE_HEADER_SP_OUTSIDE_RAM,
    IMAGE_HEADER_NOT_NEWER
} ImageHeaderStatus_t;

// What this board knows for itself, independently of the header, to compare the header with.
typedef struct {
    uint32_t hw_id;          // THIS_HARDWARE_ID
    uint32_t link_address;   // MAIN_APP_ADDR
    uint32_t image_size;     // bytes of image received (without header and tag)
    uint32_t image_crc32;    // CRC-32 computed over those bytes while they came in
    uint32_t first_word;     // the first two little-endian words of the image as received
    uint32_t second_word;
    uint32_t ram_lo;         // initial stack pointer must be in [ram_lo, ram_hi]
    uint32_t ram_hi;
} ImageHeaderExpect_t;

static inline uint32_t ImageHeader_Le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint16_t ImageHeader_Le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline void ImageHeader_PutLe16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

// Reads the 64 bytes. Refuses a header that is not ours or that has anything set where it must be zero.
static inline ImageHeaderStatus_t ImageHeader_Parse(const uint8_t raw[IMAGE_HEADER_SIZE], ImageHeader_t *out)
{
    if (ImageHeader_Le32(raw) != IMAGE_HEADER_MAGIC) return IMAGE_HEADER_BAD_MAGIC;
    if (ImageHeader_Le16(raw + 4) != IMAGE_HEADER_FORMAT) return IMAGE_HEADER_BAD_FORMAT;
    if (ImageHeader_Le16(raw + 6) != 0u || ImageHeader_Le16(raw + 18) != 0u) return IMAGE_HEADER_RESERVED_NOT_ZERO;
    for (size_t i = 40; i < IMAGE_HEADER_SIZE; i++) {
        if (raw[i] != 0u) return IMAGE_HEADER_RESERVED_NOT_ZERO;
    }
    out->hw_id = ImageHeader_Le32(raw + 8);
    out->version.major = ImageHeader_Le16(raw + 12);
    out->version.minor = ImageHeader_Le16(raw + 14);
    out->version.patch = ImageHeader_Le16(raw + 16);
    out->image_size = ImageHeader_Le32(raw + 20);
    out->link_address = ImageHeader_Le32(raw + 24);
    out->vector_sp = ImageHeader_Le32(raw + 28);
    out->vector_reset = ImageHeader_Le32(raw + 32);
    out->image_crc32 = ImageHeader_Le32(raw + 36);
    return IMAGE_HEADER_OK;
}

// Everything the header says that this board can check for itself, in the order of the host reference.
static inline ImageHeaderStatus_t ImageHeader_Check(const ImageHeader_t *h, const ImageHeaderExpect_t *e)
{
    if (h->image_size != e->image_size) return IMAGE_HEADER_SIZE_MISMATCH;
    if (h->image_crc32 != e->image_crc32) return IMAGE_HEADER_CRC_MISMATCH;
    if (h->hw_id != e->hw_id) return IMAGE_HEADER_WRONG_HARDWARE;
    if (h->link_address != e->link_address) return IMAGE_HEADER_WRONG_LINK_ADDRESS;
    if (h->vector_sp != e->first_word || h->vector_reset != e->second_word) return IMAGE_HEADER_VECTORS_MISMATCH;
    if ((h->vector_reset & 1u) == 0u) return IMAGE_HEADER_RESET_NOT_THUMB;
    uint32_t reset_address = h->vector_reset & ~1u;
    // written so that no sum can wrap around: link <= reset < link + size
    if (reset_address < h->link_address || (reset_address - h->link_address) >= h->image_size) return IMAGE_HEADER_RESET_OUTSIDE_IMAGE;
    if (h->vector_sp < e->ram_lo || h->vector_sp > e->ram_hi) return IMAGE_HEADER_SP_OUTSIDE_RAM;
    return IMAGE_HEADER_OK;
}

// -1, 0, 1 as a is older than, equal to, newer than b: (major, minor, patch) as one number, patch included.
static inline int ImageHeader_VersionCompare(const ImageVersion_t *a, const ImageVersion_t *b)
{
    if (a->major != b->major) return a->major < b->major ? -1 : 1;
    if (a->minor != b->minor) return a->minor < b->minor ? -1 : 1;
    if (a->patch != b->patch) return a->patch < b->patch ? -1 : 1;
    return 0;
}

// The anti-rollback rule: the new image must be strictly newer than the installed one. `installed` is NULL when nothing valid is
// installed. An older image passes only with `downgrade_authorized`, which the caller sets after checking a signature made for exactly
// this pair of versions in this session (ImageHeader_DowngradeMessage); the same version never passes, token or not.
static inline ImageHeaderStatus_t ImageHeader_CheckRollback(const ImageHeader_t *h, const ImageVersion_t *installed, int downgrade_authorized)
{
    if (installed == NULL) return IMAGE_HEADER_OK;
    int order = ImageHeader_VersionCompare(&h->version, installed);
    if (order > 0) return IMAGE_HEADER_OK;
    if (order < 0 && downgrade_authorized) return IMAGE_HEADER_OK;
    return IMAGE_HEADER_NOT_NEWER;
}

// The message whose Ed25519 signature under the update key is the downgrade token for going from `installed` to the older `target`
// with the image whose digest is `image_digest`, in the session the bootloader chose `nonce` for. It is useless in any other session, for
// any other pair of versions, for any other image, on any other board with another key; the bootloader builds the same message and
// verifies the signature of it, so the firmware holds no constant that allows a downgrade (the old flow had one: four magic bytes
// anybody on the bus could send) and no secret that makes one.
static inline void ImageHeader_DowngradeMessage(const uint8_t nonce[IMAGE_NONCE_SIZE], const ImageVersion_t *installed,
                                                const ImageVersion_t *target, const uint8_t image_digest[IMAGE_DIGEST_SIZE],
                                                uint8_t out[DOWNGRADE_MESSAGE_LEN])
{
    static const char tag[DOWNGRADE_TAG_LEN] = {'h', 'y', 'd', 'r', 'a', '-', 'u', 'm', 'c', '-', 'd', 'o', 'w', 'n', 'g', 'r', 'a', 'd', 'e', '\0'};
    for (size_t i = 0; i < DOWNGRADE_TAG_LEN; i++) out[i] = (uint8_t)tag[i];
    for (size_t i = 0; i < IMAGE_NONCE_SIZE; i++) out[DOWNGRADE_TAG_LEN + i] = nonce[i];
    uint8_t *p = out + DOWNGRADE_TAG_LEN + IMAGE_NONCE_SIZE;
    ImageHeader_PutLe16(p + 0, installed->major);
    ImageHeader_PutLe16(p + 2, installed->minor);
    ImageHeader_PutLe16(p + 4, installed->patch);
    ImageHeader_PutLe16(p + 6, target->major);
    ImageHeader_PutLe16(p + 8, target->minor);
    ImageHeader_PutLe16(p + 10, target->patch);
    for (size_t i = 0; i < IMAGE_DIGEST_SIZE; i++) p[12 + i] = image_digest[i];
}

// The message the signature of an image is made over: the domain text, then the SHA-256 digest of header || image.
static inline void ImageHeader_SignatureMessage(const uint8_t digest[IMAGE_DIGEST_SIZE], uint8_t out[IMAGE_MESSAGE_LEN])
{
    static const char domain[IMAGE_DOMAIN_LEN] = {'h', 'y', 'd', 'r', 'a', '-', 'u', 'm', 'c', '-', 'i', 'm', 'a', 'g', 'e', '-', 's', 'i', 'g', '\0'};
    for (size_t i = 0; i < IMAGE_DOMAIN_LEN; i++) out[i] = (uint8_t)domain[i];
    for (size_t i = 0; i < IMAGE_DIGEST_SIZE; i++) out[IMAGE_DOMAIN_LEN + i] = digest[i];
}

#endif // IMAGE_HEADER_H
