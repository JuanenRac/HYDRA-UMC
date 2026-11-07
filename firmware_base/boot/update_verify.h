// =============================================================================
// HYDRA-UMC Bootloader - The decision at the end of an update
// Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
// GPL-3.0-or-later - see repo root LICENSE
// =============================================================================
// Pure logic, no HAL and no flash access (the caller reads the flash, computes the SHA-256 digest of header || image and supplies the
// Ed25519 check), so it runs unchanged on the host (tests/test_update_verify.c, and tests/test_bootloader_g474_host.c with the real
// signature check) and in the three bootloaders. It decides, in this order, whether the image that has just arrived in the backup slot may
// be installed:
//
//   1. everything arrived: all the bytes, the eight signature chunks (64 bytes) and the eight header chunks;
//   2. the CRC-32 the host declared in END_UPDATE is the one computed over the bytes received;
//   3. the SIGNATURE, Ed25519 over IMAGE_DOMAIN || SHA-256(header || image), verifies under the update public key: nothing in the header
//      is believed before this holds;
//   4. the header is ours (magic, format, zeros), is for this hardware and this link address, has the size and CRC of what arrived,
//      and its vector words are the first two words of the image (image_header.h);
//   5. the version of END_UPDATE is the header's (it is no longer the source of the truth);
//   6. the image is strictly newer than the installed one, comparing major.minor.patch; an older one only with a downgrade token that is
//      the signature of a message made of this session's nonce, the installed and the new version and the digest of THIS image.
//
// The first step that fails is the answer; the caller turns it into the reason the bootloader reports (VERIFY_FAIL_REASON_*).
#ifndef UPDATE_VERIFY_H
#define UPDATE_VERIFY_H

#include <stddef.h>
#include <stdint.h>

#include "image_header.h"

#define UPDATE_SIGNATURE_CHUNKS 8u   // 8 x 8 bytes of signature
#define UPDATE_HEADER_CHUNKS    8u   // 8 x 8 bytes of header
#define UPDATE_TOKEN_CHUNKS     8u   // 8 x 8 bytes of downgrade token (a signature too)

typedef enum {
    UPDATE_VERDICT_OK = 0,
    UPDATE_VERDICT_INCOMPLETE,
    UPDATE_VERDICT_CRC32,
    UPDATE_VERDICT_SIGNATURE,
    UPDATE_VERDICT_HEADER,        // the header is not ours, or says something that is not true of what arrived
    UPDATE_VERDICT_HARDWARE_ID,
    UPDATE_VERDICT_ROLLBACK,
    UPDATE_VERDICT_UNKNOWN_KEY,   // header format 2: the key id names no key of the trust table (or a revoked one)
    UPDATE_VERDICT_COUNTER        // header format 2: the rollback counter is below the floor
} UpdateVerdict_t;

// Checks an Ed25519 signature of `len` bytes at `msg` under the update public key; 1 when it is valid.
typedef int (*UpdateVerifyFn)(const uint8_t *msg, uint32_t len, const uint8_t signature[IMAGE_SIGNATURE_SIZE], void *context);

typedef struct {
    // what arrived
    uint32_t bytes_received;
    uint32_t total_size;
    uint8_t  signature_chunks;
    uint8_t  header_chunks;
    uint8_t  token_chunks;
    uint32_t declared_crc;               // END_UPDATE
    uint32_t actual_crc;                 // computed over the bytes received
    uint16_t declared_major;             // END_UPDATE
    uint16_t declared_minor;
    const uint8_t *header;               // IMAGE_HEADER_SIZE bytes
    const uint8_t *signature;            // IMAGE_SIGNATURE_SIZE bytes, from the signature chunks
    const uint8_t *actual_digest;        // IMAGE_DIGEST_SIZE bytes, computed by the caller: SHA-256 over header || image
    const uint8_t *token;                // IMAGE_SIGNATURE_SIZE bytes, from the token chunks (read only if token_chunks is complete)
    const uint8_t *nonce;                // IMAGE_NONCE_SIZE bytes, the one this session announced
    // what this board knows for itself
    ImageHeaderExpect_t expect;          // hardware id, link address, RAM, and the first two words of the image as received
    int            have_installed;       // 0: nothing valid is installed
    ImageVersion_t installed;
    // the Ed25519 check under the update public key, for the image signature and for the downgrade token
    UpdateVerifyFn verify;
    void          *verify_context;
} UpdateVerifyInput_t;

// Constant-time equality, the same rule as hmac_constant_time_compare: no early exit on the first differing byte.
static inline int UpdateVerify_Equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

static inline UpdateVerdict_t UpdateVerify_Decide(const UpdateVerifyInput_t *in, ImageHeader_t *header_out)
{
    if (in->bytes_received != in->total_size || in->signature_chunks != UPDATE_SIGNATURE_CHUNKS || in->header_chunks != UPDATE_HEADER_CHUNKS) {
        return UPDATE_VERDICT_INCOMPLETE;
    }
    if (in->actual_crc != in->declared_crc) return UPDATE_VERDICT_CRC32;

    uint8_t image_message[IMAGE_MESSAGE_LEN];
    ImageHeader_SignatureMessage(in->actual_digest, image_message);
    if (in->verify == NULL || !in->verify(image_message, IMAGE_MESSAGE_LEN, in->signature, in->verify_context)) return UPDATE_VERDICT_SIGNATURE;

    ImageHeader_t header;
    if (ImageHeader_Parse(in->header, &header) != IMAGE_HEADER_OK) return UPDATE_VERDICT_HEADER;
    ImageHeaderExpect_t expect = in->expect;
    expect.image_size = in->total_size;
    expect.image_crc32 = in->actual_crc;
    ImageHeaderStatus_t status = ImageHeader_Check(&header, &expect);
    if (status == IMAGE_HEADER_WRONG_HARDWARE) return UPDATE_VERDICT_HARDWARE_ID;
    if (status != IMAGE_HEADER_OK) return UPDATE_VERDICT_HEADER;
    if (in->declared_major != header.version.major || in->declared_minor != header.version.minor) return UPDATE_VERDICT_HEADER;

    int downgrade_authorized = 0;
    if (in->have_installed && in->token_chunks == UPDATE_TOKEN_CHUNKS && in->verify != NULL) {
        uint8_t message[DOWNGRADE_MESSAGE_LEN];
        ImageHeader_DowngradeMessage(in->nonce, &in->installed, &header.version, in->actual_digest, message);
        downgrade_authorized = in->verify(message, DOWNGRADE_MESSAGE_LEN, in->token, in->verify_context) ? 1 : 0;
    }
    if (ImageHeader_CheckRollback(&header, in->have_installed ? &in->installed : NULL, downgrade_authorized) != IMAGE_HEADER_OK) {
        return UPDATE_VERDICT_ROLLBACK;
    }
    if (header_out != NULL) *header_out = header;
    return UPDATE_VERDICT_OK;
}

// ---- the decision with a trust table (header format 2) -------------------------------------------------------------------------------------------
// The same steps, with the key picked from the trust table instead of being the only one the board knows: the key id of the header names which of the
// (at most two) trusted keys must verify the signature; a key that is not in the table, or is revoked there, is refused. Order, as the host reference
// (tools/image_header2.py decide2) takes it: everything arrived; the CRC-32 of END_UPDATE; the header is ours and of format 2; the key is trusted; the
// SIGNATURE, over IMAGE_DOMAIN_2 || SHA-256(header || image); the header against what arrived; the version of END_UPDATE is the header's; the
// ROLLBACK COUNTER is not below the floor (never overridable); the version is strictly newer, or an older one with a downgrade token that is a
// signature, under the same key, of this session's nonce, the two versions and the digest of THIS image.

// Checks an Ed25519 signature of `len` bytes at `msg` under ONE public key; 1 when it is valid.
typedef int (*UpdateVerifyKeyFn)(const uint8_t *msg, uint32_t len, const uint8_t signature[IMAGE_SIGNATURE_SIZE], const uint8_t public_key[32], void *context);

typedef struct {
    // what arrived
    uint32_t bytes_received;
    uint32_t total_size;
    uint8_t  signature_chunks;
    uint8_t  header_chunks;
    uint8_t  token_chunks;
    uint32_t declared_crc;               // END_UPDATE
    uint32_t actual_crc;                 // computed over the bytes received
    uint16_t declared_major;             // END_UPDATE
    uint16_t declared_minor;
    const uint8_t *header;               // IMAGE_HEADER_SIZE bytes
    const uint8_t *signature;            // IMAGE_SIGNATURE_SIZE bytes, from the signature chunks
    const uint8_t *actual_digest;        // IMAGE_DIGEST_SIZE bytes, computed by the caller: SHA-256 over header || image
    const uint8_t *token;                // IMAGE_SIGNATURE_SIZE bytes, from the token chunks (read only if token_chunks is complete)
    const uint8_t *nonce;                // IMAGE_NONCE_SIZE bytes, the one this session announced
    // what this board knows for itself
    ImageHeaderExpect_t expect;          // hardware id, link address, RAM, and the first two words of the image as received
    int            have_installed;       // 0: nothing valid is installed
    ImageVersion_t installed;
    uint32_t       counter_floor;        // the larger of the floor kept in the metadata and the one compiled into the bootloader
    // the trusted keys (revoked ones are not here) and their ids, and the Ed25519 check under one of them
    uint8_t        key_count;
    const uint8_t (*key_ids)[IMAGE_KEY_ID_SIZE];
    const uint8_t (*keys)[32];
    UpdateVerifyKeyFn verify;
    void          *verify_context;
} UpdateVerify2Input_t;

static inline UpdateVerdict_t UpdateVerify_Decide2(const UpdateVerify2Input_t *in, ImageHeader2_t *header_out)
{
    if (in->bytes_received != in->total_size || in->signature_chunks != UPDATE_SIGNATURE_CHUNKS || in->header_chunks != UPDATE_HEADER_CHUNKS) {
        return UPDATE_VERDICT_INCOMPLETE;
    }
    if (in->actual_crc != in->declared_crc) return UPDATE_VERDICT_CRC32;

    ImageHeader2_t header;
    if (ImageHeader_Parse2(in->header, &header) != IMAGE_HEADER_OK) return UPDATE_VERDICT_HEADER;

    const uint8_t *key = NULL;
    for (uint8_t i = 0; i < in->key_count && key == NULL; i++) {
        if (UpdateVerify_Equal(in->key_ids[i], header.key_id, IMAGE_KEY_ID_SIZE)) key = in->keys[i];
    }
    if (key == NULL) return UPDATE_VERDICT_UNKNOWN_KEY;

    uint8_t image_message[IMAGE_MESSAGE_2_LEN];
    ImageHeader_SignatureMessage2(in->actual_digest, image_message);
    if (in->verify == NULL || !in->verify(image_message, IMAGE_MESSAGE_2_LEN, in->signature, key, in->verify_context)) return UPDATE_VERDICT_SIGNATURE;

    ImageHeaderExpect_t expect = in->expect;
    expect.image_size = in->total_size;
    expect.image_crc32 = in->actual_crc;
    ImageHeaderStatus_t status = ImageHeader_Check(&header.base, &expect);
    if (status == IMAGE_HEADER_WRONG_HARDWARE) return UPDATE_VERDICT_HARDWARE_ID;
    if (status != IMAGE_HEADER_OK) return UPDATE_VERDICT_HEADER;
    if (in->declared_major != header.base.version.major || in->declared_minor != header.base.version.minor) return UPDATE_VERDICT_HEADER;

    if (header.rollback_counter < in->counter_floor) return UPDATE_VERDICT_COUNTER;

    int downgrade_authorized = 0;
    if (in->have_installed && in->token_chunks == UPDATE_TOKEN_CHUNKS) {
        uint8_t message[DOWNGRADE_MESSAGE_LEN];
        ImageHeader_DowngradeMessage(in->nonce, &in->installed, &header.base.version, in->actual_digest, message);
        downgrade_authorized = in->verify(message, DOWNGRADE_MESSAGE_LEN, in->token, key, in->verify_context) ? 1 : 0;
    }
    if (ImageHeader_CheckRollback(&header.base, in->have_installed ? &in->installed : NULL, downgrade_authorized) != IMAGE_HEADER_OK) {
        return UPDATE_VERDICT_ROLLBACK;
    }
    if (header_out != NULL) *header_out = header;
    return UPDATE_VERDICT_OK;
}

#endif // UPDATE_VERIFY_H
