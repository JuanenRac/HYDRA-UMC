/* =============================================================================
 * HYDRA-UMC Firmware - host test of ipc_mailbox.h (seal of the mailbox frame and the semaphore protocol between the two cores)
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Built twice: with the default (an unsealed frame is accepted) and with -DHYDRA_IPC_REQUIRE_CRC=1 (an unsealed frame is refused), so both
 * settings of the switch are exercised. This test is about the SEAL of the frame. The semaphore handshake between the two cores (every core
 * releases only the semaphore it took) is tested in tests/test_ipc_link.c, with a double that behaves like the hardware semaphores.
 */
#include <stdio.h>
#include <string.h>
#include "ipc_mailbox.h"
#include "spi_frame_crc.h"

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } } while (0)

#if HYDRA_IPC_REQUIRE_CRC
#define UNSEALED_ACCEPTED 0u
#else
#define UNSEALED_ACCEPTED 1u
#endif

static void fill(IpcFrame_t *f, uint8_t type, uint8_t dlc, uint8_t seed)
{
    f->frame_type = type;
    f->dlc = dlc;
    for (unsigned i = 0; i < 8u; i++) f->payload[i] = (uint8_t)(seed + 31u * i);
    for (unsigned i = 0; i < 6u; i++) f->_reserved[i] = 0;
}

static void seal_tests(void)
{
    IpcFrame_t f;
    fill(&f, 0x05, 8, 7);
    CHECK(f._reserved[3] == 0 && IpcFrame_Valid(&f) == UNSEALED_ACCEPTED, "a frame without the marker follows the switch");
    IpcFrame_Seal(&f, 42);
    CHECK(f._reserved[0] == 42 && f._reserved[3] == IPC_FRAME_MARKER, "the seal writes the sequence and the marker");
    CHECK(IpcFrame_Valid(&f) == 1u, "a sealed frame is valid");

    /* the same CRC-16/CCITT-FALSE as the SPI frame, over the same 11 bytes: type, dlc, payload and sequence */
    uint8_t span[11];
    span[0] = f.frame_type;
    span[1] = f.dlc;
    memcpy(span + 2, (const void *)f.payload, 8);
    span[10] = f._reserved[0];
    CHECK(SpiWire_Crc16(span, 11) == IpcFrame_Crc(&f), "mailbox and SPI use the same CRC");
    CHECK((uint16_t)(((uint16_t)f._reserved[1] << 8) | f._reserved[2]) == IpcFrame_Crc(&f), "the CRC is stored big endian");

    /* every flipped bit of the covered bytes, and of the stored CRC, is caught; the marker is the only byte that selects the older kind */
    for (unsigned bit = 0; bit < 8u * 10u; bit++) {
        IpcFrame_t g = f;
        volatile uint8_t *bytes[10] = {&g.frame_type, &g.dlc, &g.payload[0], &g.payload[1], &g.payload[2], &g.payload[3],
                              &g.payload[4], &g.payload[5], &g.payload[6], &g.payload[7]};
        *bytes[bit / 8u] ^= (uint8_t)(1u << (bit % 8u));
        CHECK(IpcFrame_Valid(&g) == 0u, "a flipped bit of the frame is caught");
    }
    for (unsigned bit = 0; bit < 24u; bit++) {   /* sequence byte and the two CRC bytes */
        IpcFrame_t g = f;
        volatile uint8_t *bytes[3] = {&g._reserved[0], &g._reserved[1], &g._reserved[2]};
        *bytes[bit / 8u] ^= (uint8_t)(1u << (bit % 8u));
        CHECK(IpcFrame_Valid(&g) == 0u, "a flipped bit of the sequence or of the CRC is caught");
    }
    IpcFrame_t g = f;
    g._reserved[3] ^= 0x01u;   /* a marker that is not the marker is an unsealed frame: the switch decides */
    CHECK(IpcFrame_Valid(&g) == UNSEALED_ACCEPTED, "a damaged marker is an unsealed frame");

    /* a changed sequence with an old CRC is refused: a replayed answer cannot pass as the next one */
    IpcFrame_t h;
    fill(&h, 0x05, 8, 7);
    IpcFrame_Seal(&h, 43);
    CHECK(h._reserved[1] != f._reserved[1] || h._reserved[2] != f._reserved[2], "the sequence changes the CRC");
}

int main(void)
{
    seal_tests();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ipc_mailbox host tests passed\n");
    return 0;
}
