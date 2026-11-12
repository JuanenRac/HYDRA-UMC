/* =============================================================================
 * HYDRA-UMC Firmware - host test of spi_boot_master.h (the frames of the SPI link on which the bootloader is the master)
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * The vectors are those of tests/spi_boot_master_vectors.json, produced by the Python model (tools/spi_boot_master.py); tests/test_spi_boot_master_vectors.py
 * checks that the header of the vectors is what the model produces. Nothing here touches the SPI peripheral.
 */
#include <stdio.h>
#include <string.h>
#include "spi_boot_master.h"
#include "spi_boot_master_vectors.h"

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } } while (0)

static uint32_t from_hex(const char *hex, uint8_t *out, uint32_t capacity)
{
    uint32_t n = 0;
    while (hex[2u * n] != '\0' && n < capacity) {
        unsigned v = 0;
        sscanf(hex + 2u * n, "%2x", &v);
        out[n++] = (uint8_t)v;
    }
    return n;
}

int main(void)
{
    /* the CRC-32 of "123456789" is the standard check value */
    const uint8_t check[9] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK(Sbm_Crc32(check, 9) == 0xCBF43926u, "CRC-32 check value");
    CHECK(Sbm_Crc32(check, 0) == 0u, "CRC-32 of nothing");

    for (unsigned i = 0; i < sizeof SBM_FRAMES / sizeof SBM_FRAMES[0]; i++) {
        const SbmFrameVector *v = &SBM_FRAMES[i];
        uint8_t payload[SBM_MAX_PAYLOAD], want[SBM_MAX_FRAME], got[SBM_MAX_FRAME];
        const uint32_t payload_bytes = from_hex(v->payload_hex, payload, sizeof payload);
        const uint32_t frame_bytes = from_hex(v->frame_hex, want, sizeof want);
        const uint32_t built = Sbm_Encode(got, (uint8_t)v->type, (uint16_t)v->sequence, payload, (uint16_t)payload_bytes);
        CHECK(built == frame_bytes && memcmp(got, want, frame_bytes) == 0, v->name);
        uint8_t type = 0;
        uint16_t sequence = 0, length = 0;
        const uint8_t *body = 0;
        CHECK(Sbm_Decode(want, frame_bytes, v->host, &type, &sequence, &body, &length) == SBM_OK, v->name);
        CHECK(type == v->type && sequence == v->sequence && length == payload_bytes && memcmp(body, payload, payload_bytes) == 0, v->name);
    }

    for (unsigned i = 0; i < sizeof SBM_BAD_FRAMES / sizeof SBM_BAD_FRAMES[0]; i++) {
        const SbmBadFrameVector *v = &SBM_BAD_FRAMES[i];
        uint8_t frame[SBM_MAX_FRAME + 8];
        const uint32_t bytes = from_hex(v->frame_hex, frame, sizeof frame);
        uint8_t type = 0;
        uint16_t sequence = 0, length = 0;
        const uint8_t *body = 0;
        CHECK((int)Sbm_Decode(frame, bytes, v->host, &type, &sequence, &body, &length) == v->status, v->name);
    }

    /* every single bit of a good frame that is flipped is refused */
    {
        uint8_t frame[SBM_MAX_FRAME];
        const uint32_t bytes = from_hex(SBM_FRAMES[4].frame_hex, frame, sizeof frame); /* the offer */
        for (uint32_t byte = 0; byte < bytes; byte++) {
            for (int bit = 0; bit < 8; bit++) {
                uint8_t copy[SBM_MAX_FRAME];
                memcpy(copy, frame, bytes);
                copy[byte] ^= (uint8_t)(1u << bit);
                uint8_t type = 0;
                uint16_t sequence = 0, length = 0;
                const uint8_t *body = 0;
                CHECK(Sbm_Decode(copy, bytes, 1, &type, &sequence, &body, &length) != SBM_OK, "a flipped bit is accepted");
            }
        }
    }

    /* a payload that is too long is not encoded */
    {
        uint8_t big[SBM_MAX_PAYLOAD + 1] = {0}, out[SBM_MAX_FRAME + 8];
        CHECK(Sbm_Encode(out, SBM_T_PAGE, 1, big, (uint16_t)(SBM_MAX_PAYLOAD + 1u)) == 0u, "too long a payload");
        CHECK(Sbm_Encode(out, SBM_T_PAGE, 1, big, (uint16_t)SBM_MAX_PAYLOAD) == SBM_OVERHEAD + SBM_MAX_PAYLOAD, "the longest payload");
    }

    /* HELLO and GET_PAGE are built the way the model builds them */
    {
        uint8_t nonce[SBM_NONCE_BYTES], want[SBM_MAX_FRAME], got[SBM_MAX_FRAME];
        for (uint32_t i = 0; i < SBM_NONCE_BYTES; i++) nonce[i] = (uint8_t)i;
        const uint32_t built = Sbm_BuildHello(got, (uint16_t)SBM_HELLO_SEQUENCE, SBM_HELLO_HW_ID, SBM_HELLO_LINK, SBM_HELLO_INSTALLED, nonce, SBM_HELLO_BOOTLOADER);
        const uint32_t bytes = from_hex(SBM_FRAMES[0].frame_hex, want, sizeof want);
        CHECK(built == bytes && memcmp(got, want, bytes) == 0, "hello");
        const uint32_t page = Sbm_BuildGetPage(got, 2, 512, 256);
        const uint32_t page_bytes = from_hex(SBM_FRAMES[1].frame_hex, want, sizeof want);
        CHECK(page == page_bytes && memcmp(got, want, page_bytes) == 0, "get_page");
    }

    /* the offers: parsed and judged as the model judges them */
    for (unsigned i = 0; i < sizeof SBM_OFFERS / sizeof SBM_OFFERS[0]; i++) {
        const SbmOfferVector *v = &SBM_OFFERS[i];
        uint8_t payload[SBM_OFFER_BYTES];
        SbmOffer offer;
        CHECK(from_hex(v->payload_hex, payload, sizeof payload) == SBM_OFFER_BYTES, v->name);
        CHECK(Sbm_ParseOffer(payload, SBM_OFFER_BYTES, &offer) == 1, v->name);
        CHECK(offer.size == v->size && strcmp(offer.target, v->target) == 0, v->name);
        CHECK((int)Sbm_JudgeOffer(&offer, SBM_JUDGE_MAX_IMAGE_BYTES, SBM_JUDGE_BOARD_TARGET, SBM_JUDGE_BOOTLOADER) == v->verdict, v->name);
    }
    {
        SbmOffer offer;
        uint8_t payload[SBM_OFFER_BYTES + 1] = {0};
        CHECK(Sbm_ParseOffer(payload, SBM_OFFER_BYTES - 1u, &offer) == 0, "a short offer");
        CHECK(Sbm_ParseOffer(payload, SBM_OFFER_BYTES + 1u, &offer) == 0, "a long offer");
        /* a target name that fills all 16 bytes has no terminator in the payload: the parser adds its own */
        memset(payload, 'A', SBM_OFFER_BYTES);
        CHECK(Sbm_ParseOffer(payload, SBM_OFFER_BYTES, &offer) == 1 && strlen(offer.target) == SBM_TARGET_BYTES, "a full target name");
    }

    if (failures == 0) {
        printf("spi_boot_master host test: all checks passed\n");
        return 0;
    }
    printf("spi_boot_master host test: %d check(s) FAILED\n", failures);
    return 1;
}
