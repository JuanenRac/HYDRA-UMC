/* =============================================================================
 * HYDRA-UMC Firmware - host test of spi_frame_crc.h (CRC16 and sequence of the SPI frame)
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * The vectors are the ones of tests/spi_frame_vectors.json; tests/test_spi_frame.py checks that the two lists are the same.
 */
#include <stdio.h>
#include <string.h>
#include "spi_frame_crc.h"

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } } while (0)

static void from_hex(const char *hex, uint8_t *out, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; i++) {
        unsigned v = 0;
        sscanf(hex + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

static void check_vector(const char *name, const char *head_hex, unsigned crc)
{
    uint8_t wire[128];
    uint8_t head[16];
    from_hex(head_hex, head, 16);
    memset(wire, 0xFF, sizeof wire);
    memcpy(wire, head, 12);
    SpiWire_Seal(wire, head[SPI_WIRE_OFS_SEQ]);
    CHECK(memcmp(wire, head, 16) == 0, name);
    CHECK((unsigned)SpiWire_Crc16(wire, SPI_WIRE_CRC_SPAN) == crc, name);
    uint8_t seq = 0;
    CHECK(SpiWire_Check(wire, &seq) == SPI_WIRE_SEALED_OK && seq == head[SPI_WIRE_OFS_SEQ], name);
}

int main(void)
{
    const uint8_t check_input[9] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK(SpiWire_Crc16(check_input, 9) == 0x29B1, "CRC-16/CCITT-FALSE check value");

    check_vector("sealed_data_frame", "000005080102030405060708a1078d74", 0x8d74);
    check_vector("sealed_empty_sequence_zero", "020310000000000000000000a1008912", 0x8912);
    check_vector("sealed_sequence_255", "01000202dead000000000000a1ff39dc", 0x39dc);

    uint8_t wire[128];
    memset(wire, 0xFF, sizeof wire);
    uint8_t seq = 0;
    CHECK(SpiWire_Check(wire, &seq) == SPI_WIRE_UNSEALED, "the padding of the older kind is not sealed");
    CHECK(SpiWire_Judge(wire, 0, 0, &seq) == SPI_ACCEPT_UNSEALED, "older frame is left to the caller");

    memset(wire, 0, 12);
    SpiWire_Seal(wire, 9);
    CHECK(SpiWire_Judge(wire, 0, 0, &seq) == SPI_ACCEPT_DISPATCH, "first sealed frame runs");
    CHECK(SpiWire_Judge(wire, 1, 9, &seq) == SPI_ACCEPT_DUPLICATE, "same sequence is a retry");
    CHECK(SpiWire_Judge(wire, 1, 8, &seq) == SPI_ACCEPT_DISPATCH, "next sequence runs");
    CHECK(SpiWire_Judge(wire, 1, 3, &seq) == SPI_ACCEPT_DISPATCH, "a jump of the sequence still runs");

    for (unsigned bit = 0; bit < 8 * 14; bit++) {   /* every single flipped bit of the covered bytes is caught */
        if (bit / 8 == SPI_WIRE_OFS_MARKER) continue;   /* a damaged marker reads as a frame of the older kind */
        uint8_t damaged[128];
        memcpy(damaged, wire, sizeof damaged);
        damaged[bit / 8] ^= (uint8_t)(1u << (bit % 8));
        CHECK(SpiWire_Judge(damaged, 0, 0, &seq) == SPI_ACCEPT_BAD_CRC, "a flipped bit is caught");
    }
    for (unsigned bit = 0; bit < 16; bit++) {       /* and every flipped bit of the CRC itself */
        uint8_t damaged[128];
        memcpy(damaged, wire, sizeof damaged);
        damaged[SPI_WIRE_OFS_CRC + bit / 8] ^= (uint8_t)(1u << (bit % 8));
        CHECK(SpiWire_Judge(damaged, 0, 0, &seq) == SPI_ACCEPT_BAD_CRC, "a flipped bit of the CRC is caught");
    }

    printf("%s\n", failures ? "spi_frame_crc: FAILED" : "spi_frame_crc: all passed");
    return failures ? 1 : 0;
}
