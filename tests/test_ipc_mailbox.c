/* =============================================================================
 * HYDRA-UMC Firmware - host test of ipc_mailbox.h (seal of the mailbox frame and the semaphore protocol between the two cores)
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Built twice: with the default (an unsealed frame is accepted) and with -DHYDRA_IPC_REQUIRE_CRC=1 (an unsealed frame is refused), so both
 * settings of the switch are exercised. The second half models the two cores and the two semaphore channels with the real seal functions:
 * simulator evidence only, the timing of the real semaphore hardware stays a bench row.
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

/* The two cores and the two channels. The writer of a channel takes it after the frame is complete; the reader releases it after it has copied
 * the frame. A writer that finds its channel taken waits (it does not overwrite). */
typedef struct {
    IpcMailbox_t box;
    int taken[2];
    uint8_t cmd_seq, resp_seq;
} Model;

static int model_post_cmd(Model *m, uint8_t type, uint8_t seed)
{
    if (m->taken[IPC_HSEM_CMD_ID]) return 0;
    fill(&m->box.cmd, type, 8, seed);
    IpcFrame_Seal(&m->box.cmd, m->cmd_seq++);
    m->taken[IPC_HSEM_CMD_ID] = 1;
    return 1;
}

static int model_take_cmd(Model *m, IpcFrame_t *out)
{
    if (!m->taken[IPC_HSEM_CMD_ID]) return 0;
    *out = m->box.cmd;
    const int ok = IpcFrame_Valid(&m->box.cmd);
    m->taken[IPC_HSEM_CMD_ID] = 0;
    return ok ? 1 : -1;
}

static int model_post_resp(Model *m, uint8_t type, uint8_t seed)
{
    if (m->taken[IPC_HSEM_RESP_ID]) return 0;
    fill(&m->box.resp, type, 8, seed);
    IpcFrame_Seal(&m->box.resp, m->resp_seq++);
    m->taken[IPC_HSEM_RESP_ID] = 1;
    return 1;
}

static int model_take_resp(Model *m, IpcFrame_t *out)
{
    if (!m->taken[IPC_HSEM_RESP_ID]) return 0;
    *out = m->box.resp;
    const int ok = IpcFrame_Valid(&m->box.resp);
    m->taken[IPC_HSEM_RESP_ID] = 0;
    return ok ? 1 : -1;
}

static unsigned lcg(unsigned *state)
{
    *state = *state * 1664525u + 1013904223u;
    return *state >> 16;
}

static void protocol_tests(void)
{
    Model m;
    memset(&m, 0, sizeof m);
    IpcFrame_t got;

    /* a second command while the first is not consumed waits and does not overwrite it */
    CHECK(model_post_cmd(&m, 1, 10) == 1, "first command posted");
    CHECK(model_post_cmd(&m, 2, 20) == 0, "a second command waits");
    CHECK(model_take_cmd(&m, &got) == 1 && got.frame_type == 1 && got.payload[0] == 10, "the first command arrives intact");
    CHECK(model_take_cmd(&m, &got) == 0, "nothing is left to take twice");
    CHECK(model_post_cmd(&m, 2, 20) == 1, "the next command goes once the channel is free");

    /* several answers to one command: each waits for the reader, none is lost or overwritten */
    CHECK(model_take_cmd(&m, &got) == 1 && got.frame_type == 2, "second command consumed");
    CHECK(model_post_resp(&m, 0x10, 1) == 1, "first answer posted");
    CHECK(model_post_resp(&m, 0x11, 2) == 0, "second answer waits for the reader");
    CHECK(model_take_resp(&m, &got) == 1 && got.frame_type == 0x10 && got.payload[0] == 1, "first answer arrives intact");
    CHECK(model_post_resp(&m, 0x11, 2) == 1, "second answer goes after the first is read");
    CHECK(model_take_resp(&m, &got) == 1 && got.frame_type == 0x11 && got.payload[0] == 2, "second answer arrives intact");

    /* a frame damaged in the shared memory after the take is refused, and the channel is still freed so the link does not stall */
    CHECK(model_post_cmd(&m, 3, 30) == 1, "command posted");
    m.box.cmd.payload[4] ^= 0x40u;
    CHECK(model_take_cmd(&m, &got) == -1, "a damaged command is refused");
    CHECK(m.taken[IPC_HSEM_CMD_ID] == 0, "the channel is free after a refused frame");

    /* many random interleavings of the two cores: the answers come in the order they were posted, none repeats, none is lost */
    unsigned state = 12345u;
    unsigned posted = 0, read = 0;
    uint8_t next_post = 0, next_read = 0;
    memset(&m, 0, sizeof m);
    for (unsigned step = 0; step < 20000u; step++) {
        if (lcg(&state) & 1u) {
            if (model_post_resp(&m, next_post, next_post)) { next_post++; posted++; }
        } else if (model_take_resp(&m, &got) == 1) {
            CHECK(got.frame_type == next_read && got.payload[0] == next_read, "answers keep their order");
            next_read++;
            read++;
        }
    }
    while (model_take_resp(&m, &got) == 1) { CHECK(got.frame_type == next_read, "last answer in order"); next_read++; read++; }
    CHECK(posted == read && posted > 1000u, "every posted answer was read exactly once");
}

int main(void)
{
    seal_tests();
    protocol_tests();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ipc_mailbox host tests passed\n");
    return 0;
}
