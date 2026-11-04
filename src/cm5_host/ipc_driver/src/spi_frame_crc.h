/* =============================================================================
 * HYDRA-UMC - spi_frame_crc.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Integrity of the 128-byte frame on the SPI link between the CM5 and the H745 (the bootloader of the CM4 on one end, the driver of the CM5 on
 * the other). The bytes after the 12 of the frame were padding of 0xFF; the first four of them now carry:
 *
 *     offset 12   marker      0xA1 when the frame is sealed (the padding value 0xFF means a frame of the older kind)
 *     offset 13   sequence    one more than the sequence of the last new frame of the same sender; a retry repeats it
 *     offset 14   CRC16 high  CRC-16/CCITT-FALSE (polynomial 0x1021, start value 0xFFFF, no reflection, no final xor) of the bytes 0 to 13
 *     offset 15   CRC16 low
 *
 * Header-only and without any HAL, so a host test compiles it with the plain C compiler. The same file lives in the bootloader of the CM4 and in
 * the driver of the CM5: tests/test_spi_frame.py fails when the two copies differ. tools/spi_frame.py is the reference in Python and
 * tests/spi_frame_vectors.json the vectors both are checked against.
 * ========================================================================== */
#ifndef HYDRA_UMC_SPI_FRAME_CRC_H
#define HYDRA_UMC_SPI_FRAME_CRC_H

#include <stdint.h>

#define SPI_WIRE_MARKER      0xA1u
#define SPI_WIRE_OFS_MARKER  12u
#define SPI_WIRE_OFS_SEQ     13u
#define SPI_WIRE_OFS_CRC     14u
#define SPI_WIRE_CRC_SPAN    14u  /* the bytes 0 to 13 */

typedef enum {
    SPI_WIRE_UNSEALED = 0, /* padding of the older kind: no marker */
    SPI_WIRE_SEALED_OK = 1,
    SPI_WIRE_SEALED_BAD = 2  /* marker present, CRC does not match */
} SpiWireState;

static inline uint16_t SpiWire_Crc16(const uint8_t *data, uint32_t length)
{
    uint16_t crc = 0xFFFFu;
    for (uint32_t i = 0; i < length; i++) {
        crc = (uint16_t)(crc ^ ((uint16_t)data[i] << 8));
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

/* Writes marker, sequence and CRC into a frame whose first 13 bytes are already filled. */
static inline void SpiWire_Seal(uint8_t *wire, uint8_t sequence)
{
    wire[SPI_WIRE_OFS_MARKER] = (uint8_t)SPI_WIRE_MARKER;
    wire[SPI_WIRE_OFS_SEQ] = sequence;
    const uint16_t crc = SpiWire_Crc16(wire, SPI_WIRE_CRC_SPAN);
    wire[SPI_WIRE_OFS_CRC] = (uint8_t)(crc >> 8);
    wire[SPI_WIRE_OFS_CRC + 1u] = (uint8_t)(crc & 0xFFu);
}

static inline SpiWireState SpiWire_Check(const uint8_t *wire, uint8_t *sequence)
{
    if (wire[SPI_WIRE_OFS_MARKER] != SPI_WIRE_MARKER) return SPI_WIRE_UNSEALED;
    const uint16_t crc = SpiWire_Crc16(wire, SPI_WIRE_CRC_SPAN);
    const uint16_t stored = (uint16_t)(((uint16_t)wire[SPI_WIRE_OFS_CRC] << 8) | wire[SPI_WIRE_OFS_CRC + 1u]);
    if (crc != stored) return SPI_WIRE_SEALED_BAD;
    if (sequence) *sequence = wire[SPI_WIRE_OFS_SEQ];
    return SPI_WIRE_SEALED_OK;
}

/* What the receiver does with a frame. `have_last` is 0 until a sealed frame was accepted; `last` is the sequence of the last accepted one.
 * A repeated sequence is a retry of a frame already handled: the answer that was staged for it goes out again and the frame is not dispatched
 * twice. A jump of the sequence is accepted (frames can be lost) and counted by the caller. */
typedef enum {
    SPI_ACCEPT_DISPATCH = 0,  /* new frame, run it */
    SPI_ACCEPT_DUPLICATE = 1, /* retry of the last frame, do not run it again */
    SPI_ACCEPT_BAD_CRC = 2,   /* damaged, drop it */
    SPI_ACCEPT_UNSEALED = 3   /* older kind of frame without marker: the caller decides (HYDRA_SPI_REQUIRE_CRC) */
} SpiAccept;

static inline SpiAccept SpiWire_Judge(const uint8_t *wire, uint8_t have_last, uint8_t last, uint8_t *sequence)
{
    const SpiWireState state = SpiWire_Check(wire, sequence);
    if (state == SPI_WIRE_UNSEALED) return SPI_ACCEPT_UNSEALED;
    if (state == SPI_WIRE_SEALED_BAD) return SPI_ACCEPT_BAD_CRC;
    if (have_last && *sequence == last) return SPI_ACCEPT_DUPLICATE;
    return SPI_ACCEPT_DISPATCH;
}

#endif /* HYDRA_UMC_SPI_FRAME_CRC_H */
