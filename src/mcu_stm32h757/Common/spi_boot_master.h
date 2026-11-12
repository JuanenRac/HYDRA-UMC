/* =============================================================================
 * HYDRA-UMC - spi_boot_master.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * The frames of the SPI link on which the bootloader is the master and asks the host (the Jetson) for a signed image: docs/SPI_BOOT_MASTER.md.
 * Header-only and without any HAL, so a host test compiles it with the plain C compiler. tools/spi_boot_master.py is the reference in Python and
 * tests/spi_boot_master_vectors.json the vectors both are checked against (tools/gen_spi_boot_vectors.py writes them).
 *
 * This file only builds and reads frames and judges an offer. It never decides that an image is valid: that stays with the verification of the
 * bootloader (update_verify.h), the same chain of trust as over CAN, and the slot where the image waits is not valid until it has passed. The mode is
 * compiled in only when HYDRA_BOOT_SPI_MASTER is set; the driver that moves the bytes on SPI1 includes this header and is not part of it.
 *
 * Frame, little endian: sync 0x53, type, sequence u16, length u16, payload (at most 260), CRC-32 of every byte before it.
 * ========================================================================== */
#ifndef HYDRA_UMC_SPI_BOOT_MASTER_H
#define HYDRA_UMC_SPI_BOOT_MASTER_H

#include <stdint.h>

#define SBM_SYNC           0x53u
#define SBM_PAGE_BYTES     256u
#define SBM_MAX_PAYLOAD    (SBM_PAGE_BYTES + 4u)
#define SBM_HEADER_BYTES   6u
#define SBM_OVERHEAD       (SBM_HEADER_BYTES + 4u)
#define SBM_MAX_FRAME      (SBM_OVERHEAD + SBM_MAX_PAYLOAD)
#define SBM_OFFER_BYTES    32u
#define SBM_TARGET_BYTES   16u
#define SBM_NONCE_BYTES    16u
#define SBM_IMAGE_OVERHEAD 128u /* the signed header (64) and the signature (64) around the image */

#define SBM_T_HELLO     0x01u
#define SBM_T_GET_PAGE  0x02u
#define SBM_T_GET_TOKEN 0x03u
#define SBM_T_DONE      0x04u
#define SBM_T_OFFER     0x81u
#define SBM_T_PAGE      0x82u
#define SBM_T_TOKEN     0x83u
#define SBM_T_REFUSE    0x8Fu

typedef enum {
    SBM_OK = 0,
    SBM_ERR_SHORT = 1,
    SBM_ERR_CRC = 2,
    SBM_ERR_SYNC = 3,
    SBM_ERR_LENGTH = 4,
    SBM_ERR_TYPE = 5
} SbmStatus;

typedef enum {
    SBM_OFFER_OK = 0,
    SBM_OFFER_BAD_SIZE = 1,
    SBM_OFFER_WRONG_TARGET = 2,
    SBM_OFFER_BOOTLOADER_TOO_OLD = 3
} SbmOfferVerdict;

typedef struct {
    uint32_t size;
    uint16_t version[3];
    uint16_t min_bootloader[3]; /* 0.0.0: the image asks for no minimum */
    char target[SBM_TARGET_BYTES + 1u];
} SbmOffer;

/* CRC-32 (the one of zlib: polynomial 0xEDB88320 reflected, start 0xFFFFFFFF, final xor). */
static inline uint32_t Sbm_Crc32(const uint8_t *data, uint32_t length)
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

static inline void Sbm_Put16(uint8_t *out, uint16_t v) { out[0] = (uint8_t)(v & 0xFFu); out[1] = (uint8_t)(v >> 8); }
static inline void Sbm_Put32(uint8_t *out, uint32_t v)
{
    out[0] = (uint8_t)(v & 0xFFu); out[1] = (uint8_t)((v >> 8) & 0xFFu); out[2] = (uint8_t)((v >> 16) & 0xFFu); out[3] = (uint8_t)(v >> 24);
}
static inline uint16_t Sbm_Get16(const uint8_t *in) { return (uint16_t)(in[0] | ((uint16_t)in[1] << 8)); }
static inline uint32_t Sbm_Get32(const uint8_t *in)
{
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
}

/* Writes a frame into `out` (room for SBM_MAX_FRAME bytes) and returns its length, or 0 when the payload is too long. */
static inline uint32_t Sbm_Encode(uint8_t *out, uint8_t type, uint16_t sequence, const uint8_t *payload, uint16_t length)
{
    if (length > SBM_MAX_PAYLOAD) return 0u;
    out[0] = (uint8_t)SBM_SYNC;
    out[1] = type;
    Sbm_Put16(out + 2, sequence);
    Sbm_Put16(out + 4, length);
    for (uint16_t i = 0; i < length; i++) out[SBM_HEADER_BYTES + i] = payload[i];
    Sbm_Put32(out + SBM_HEADER_BYTES + length, Sbm_Crc32(out, SBM_HEADER_BYTES + length));
    return SBM_OVERHEAD + length;
}

/* Reads a frame that must be of a type the caller expects (`want_host`: a frame from the host, 0x80 and above; otherwise one from the master).
 * The CRC is checked before anything else is believed. */
static inline SbmStatus Sbm_Decode(const uint8_t *frame, uint32_t bytes, int want_host, uint8_t *type, uint16_t *sequence, const uint8_t **payload, uint16_t *length)
{
    if (bytes < SBM_OVERHEAD) return SBM_ERR_SHORT;
    if (Sbm_Get32(frame + bytes - 4u) != Sbm_Crc32(frame, bytes - 4u)) return SBM_ERR_CRC;
    if (frame[0] != SBM_SYNC) return SBM_ERR_SYNC;
    const uint16_t len = Sbm_Get16(frame + 4);
    if (len > SBM_MAX_PAYLOAD || bytes != (uint32_t)SBM_OVERHEAD + len) return SBM_ERR_LENGTH;
    const uint8_t t = frame[1];
    if (want_host) {
        if (t != SBM_T_OFFER && t != SBM_T_PAGE && t != SBM_T_TOKEN && t != SBM_T_REFUSE) return SBM_ERR_TYPE;
    } else {
        if (t != SBM_T_HELLO && t != SBM_T_GET_PAGE && t != SBM_T_GET_TOKEN && t != SBM_T_DONE) return SBM_ERR_TYPE;
    }
    *type = t;
    *sequence = Sbm_Get16(frame + 2);
    *payload = frame + SBM_HEADER_BYTES;
    *length = len;
    return SBM_OK;
}

/* HELLO: hardware id, link address, installed version, the nonce of the session and the version of the bootloader (36 bytes). */
static inline uint32_t Sbm_BuildHello(uint8_t *out, uint16_t sequence, uint32_t hw_id, uint32_t link_address, const uint16_t installed[3], const uint8_t nonce[SBM_NONCE_BYTES], const uint16_t bootloader[3])
{
    uint8_t payload[36];
    Sbm_Put32(payload, hw_id);
    Sbm_Put32(payload + 4, link_address);
    for (uint32_t i = 0; i < 3u; i++) Sbm_Put16(payload + 8 + 2u * i, installed[i]);
    for (uint32_t i = 0; i < SBM_NONCE_BYTES; i++) payload[14 + i] = nonce[i];
    for (uint32_t i = 0; i < 3u; i++) Sbm_Put16(payload + 30 + 2u * i, bootloader[i]);
    return Sbm_Encode(out, SBM_T_HELLO, sequence, payload, (uint16_t)sizeof payload);
}

static inline uint32_t Sbm_BuildGetPage(uint8_t *out, uint16_t sequence, uint32_t offset, uint32_t length)
{
    uint8_t payload[8];
    Sbm_Put32(payload, offset);
    Sbm_Put32(payload + 4, length);
    return Sbm_Encode(out, SBM_T_GET_PAGE, sequence, payload, (uint16_t)sizeof payload);
}

/* OFFER payload: size u32, version 3 x u16, minimum bootloader 3 x u16, target name 16 bytes. Returns 0 when it is not 32 bytes. */
static inline int Sbm_ParseOffer(const uint8_t *payload, uint16_t length, SbmOffer *offer)
{
    if (length != SBM_OFFER_BYTES) return 0;
    offer->size = Sbm_Get32(payload);
    for (uint32_t i = 0; i < 3u; i++) {
        offer->version[i] = Sbm_Get16(payload + 4 + 2u * i);
        offer->min_bootloader[i] = Sbm_Get16(payload + 10 + 2u * i);
    }
    uint32_t n = 0;
    while (n < SBM_TARGET_BYTES && payload[16 + n] != 0u) {
        offer->target[n] = (char)payload[16 + n];
        n++;
    }
    offer->target[n] = '\0';
    return 1;
}

static inline int Sbm_VersionGreater(const uint16_t a[3], const uint16_t b[3])
{
    for (uint32_t i = 0; i < 3u; i++) {
        if (a[i] != b[i]) return a[i] > b[i];
    }
    return 0;
}

/* What the bootloader decides about an offer before it fetches a byte. Nothing here replaces the verification of the whole image. */
static inline SbmOfferVerdict Sbm_JudgeOffer(const SbmOffer *offer, uint32_t max_image_bytes, const char *board_target, const uint16_t bootloader[3])
{
    if (offer->size < SBM_IMAGE_OVERHEAD || offer->size > max_image_bytes) return SBM_OFFER_BAD_SIZE;
    if (offer->target[0] != '\0') {
        uint32_t i = 0;
        while (offer->target[i] != '\0' && board_target[i] != '\0' && offer->target[i] == board_target[i]) i++;
        if (offer->target[i] != board_target[i]) return SBM_OFFER_WRONG_TARGET;
    }
    static const uint16_t none[3] = {0u, 0u, 0u};
    if (Sbm_VersionGreater(offer->min_bootloader, none) && Sbm_VersionGreater(offer->min_bootloader, bootloader)) return SBM_OFFER_BOOTLOADER_TOO_OLD;
    return SBM_OFFER_OK;
}

#endif /* HYDRA_UMC_SPI_BOOT_MASTER_H */
