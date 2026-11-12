/* =============================================================================
 * HYDRA-UMC Firmware - host test of ipc_link.h (the operations on the mailbox between the two cores, for whichever core owns the host link)
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Both cores are played by one thread over a mailbox that lives in the test: the gateway posts a command and waits for the answer, the backend takes the command and posts the
 * answer. The semaphores, the data barrier, the tick and the watchdog refresh are doubles that log what happens, so the ORDER of the steps (the frame is written before the barrier,
 * the barrier before the semaphore is taken) and the rules on time-outs are checked. Built for both platforms (the gateway is the CM4 on the H745 platform and the CM7 on the
 * H757 one) and with HYDRA_IPC_REQUIRE_CRC off and on. Nothing here touches a semaphore or shared memory.
 */
#include <stdio.h>
#include <string.h>
#include "ipc_link.h"

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } } while (0)

typedef struct {
    int taken[2];
    uint32_t now;
    unsigned refreshes;
    char log[64];              /* the order of the barrier and the takes: 'B' barrier, 'T' take, 'R' release */
    unsigned log_length;
    int never_release;         /* the other core never consumes the frame */
} World;

static void note(World *w, char c) { if (w->log_length < sizeof w->log - 1u) w->log[w->log_length++] = c; }
static uint32_t op_tick(void *ctx) { World *w = (World *)ctx; return w->now++; }
static void op_refresh(void *ctx) { ((World *)ctx)->refreshes++; }
static int op_taken(void *ctx, uint32_t channel) { return ((World *)ctx)->taken[channel]; }
static void op_take(void *ctx, uint32_t channel) { World *w = (World *)ctx; w->taken[channel] = 1; note(w, 'T'); }
static void op_release(void *ctx, uint32_t channel) { World *w = (World *)ctx; if (!w->never_release) w->taken[channel] = 0; note(w, 'R'); }
static void op_barrier(void *ctx) { note((World *)ctx, 'B'); }

static const IpcOps OPS = {op_tick, op_refresh, op_taken, op_take, op_release, op_barrier};

static void world_init(World *w) { memset(w, 0, sizeof *w); }

int main(void)
{
    static IpcMailbox_t mailbox;
    World w;
    IpcMessage got;
    uint8_t gateway_sequence = 0, backend_sequence = 0;
    const uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};

    /* who is the gateway: the owner of SPI1, FDCAN1 and I2C4 in the platform profile */
#if defined(HYDRA_PLATFORM_H757)
    CHECK(IPC_GATEWAY_CORE == HYDRA_OWNER_CM7 && IPC_BACKEND_CORE == HYDRA_OWNER_CM4, "on the H757 platform the CM7 is the gateway");
#else
    CHECK(IPC_GATEWAY_CORE == HYDRA_OWNER_CM4 && IPC_BACKEND_CORE == HYDRA_OWNER_CM7, "on the H745 platform the CM4 is the gateway");
#endif

    /* a round trip: command, answer, both semaphores free again, sequences rising */
    world_init(&w);
    memset((void *)&mailbox, 0, sizeof mailbox);
    CHECK(Ipc_PostCommand(&OPS, &w, &mailbox, &gateway_sequence, 0x05, payload, 8) == IPC_OK, "post the command");
    CHECK(w.taken[IPC_HSEM_CMD_ID] == 1, "the command semaphore is taken");
    CHECK(Ipc_TakeCommand(&OPS, &w, &mailbox, &got) == IPC_OK && got.frame_type == 0x05 && got.dlc == 8 && memcmp(got.payload, payload, 8) == 0, "the backend takes the command as it was posted");
    CHECK(w.taken[IPC_HSEM_CMD_ID] == 0, "the command semaphore is released after the take");
    const uint8_t answer[2] = {0x10, 0x20};
    CHECK(Ipc_PostResponse(&OPS, &w, &mailbox, &backend_sequence, 0x0F, answer, 2) == IPC_OK, "post the answer");
    CHECK(Ipc_WaitResponse(&OPS, &w, &mailbox, &got) == IPC_OK && got.frame_type == 0x0F && got.dlc == 2 && got.payload[0] == 0x10 && got.payload[1] == 0x20 && got.payload[2] == 0, "the gateway gets the answer, padded with zeros");
    CHECK(w.taken[IPC_HSEM_RESP_ID] == 0, "the answer semaphore is released after the take");
    CHECK(gateway_sequence == 1 && backend_sequence == 1, "each sequence number rose by one");
    CHECK(mailbox.cmd._reserved[0] == 0 && mailbox.resp._reserved[0] == 0, "the sequence number sits in the frame");
    CHECK(strcmp(w.log, "BTRBTR") == 0, "the order is: barrier, take, release, barrier, take, release (the barrier always comes before a take)");

    /* a second command posted while the first is still not consumed: busy, and the frame is NOT touched */
    world_init(&w);
    memset((void *)&mailbox, 0, sizeof mailbox);
    CHECK(Ipc_PostCommand(&OPS, &w, &mailbox, &gateway_sequence, 0x11, payload, 8) == IPC_OK, "first command");
    const uint8_t second[8] = {9, 9, 9, 9, 9, 9, 9, 9};
    const uint8_t seq_before = gateway_sequence;
    CHECK(Ipc_PostCommand(&OPS, &w, &mailbox, &gateway_sequence, 0x22, second, 8) == IPC_BUSY, "second command: busy");
    CHECK(mailbox.cmd.frame_type == 0x11 && mailbox.cmd.payload[0] == 1 && gateway_sequence == seq_before, "the frame and the sequence are untouched");
    CHECK(w.refreshes > 0u, "the watchdog is refreshed while waiting");
    CHECK(w.now >= IPC_POST_COMMAND_WAIT_MS, "it waited for the time before giving up");

    /* a command damaged in the shared memory is reported and dropped, and the semaphore is still released */
    world_init(&w);
    memset((void *)&mailbox, 0, sizeof mailbox);
    CHECK(Ipc_PostCommand(&OPS, &w, &mailbox, &gateway_sequence, 0x33, payload, 8) == IPC_OK, "post");
    mailbox.cmd.payload[3] ^= 0x40;
    CHECK(Ipc_TakeCommand(&OPS, &w, &mailbox, &got) == IPC_DAMAGED && w.taken[IPC_HSEM_CMD_ID] == 0, "a damaged command is reported and the semaphore released");

    /* nothing posted: nothing taken */
    world_init(&w);
    CHECK(Ipc_TakeCommand(&OPS, &w, &mailbox, &got) == IPC_NONE, "no command: none");

    /* no answer: time-out, after the time */
    world_init(&w);
    CHECK(Ipc_WaitResponse(&OPS, &w, &mailbox, &got) == IPC_TIMEOUT && w.now >= IPC_WAIT_RESPONSE_MS && w.refreshes > 0u, "no answer: time-out, with the watchdog refreshed");

    /* a damaged answer is reported, and the semaphore is released */
    world_init(&w);
    memset((void *)&mailbox, 0, sizeof mailbox);
    CHECK(Ipc_PostResponse(&OPS, &w, &mailbox, &backend_sequence, 0x0F, answer, 2) == IPC_OK, "post the answer");
    mailbox.resp.dlc ^= 0x01;
    CHECK(Ipc_WaitResponse(&OPS, &w, &mailbox, &got) == IPC_DAMAGED && w.taken[IPC_HSEM_RESP_ID] == 0, "a damaged answer is reported and released");

    /* the gateway does not drain the answer: the backend drops the next one instead of blocking, and does not touch the frame */
    world_init(&w);
    memset((void *)&mailbox, 0, sizeof mailbox);
    CHECK(Ipc_PostResponse(&OPS, &w, &mailbox, &backend_sequence, 0x0A, answer, 2) == IPC_OK, "first answer");
    CHECK(Ipc_PostResponse(&OPS, &w, &mailbox, &backend_sequence, 0x0B, answer, 2) == IPC_BUSY && mailbox.resp.frame_type == 0x0A, "second answer: dropped, the first is intact");

    /* a length beyond 8 is clamped to the payload and the rest is zero */
    world_init(&w);
    memset((void *)&mailbox, 0, sizeof mailbox);
    CHECK(Ipc_PostCommand(&OPS, &w, &mailbox, &gateway_sequence, 0x44, payload, 200) == IPC_OK && mailbox.cmd.payload[7] == 8, "a long length is clamped to the 8 bytes of payload");
    memset((void *)&mailbox, 0, sizeof mailbox);
    world_init(&w);
    CHECK(Ipc_PostCommand(&OPS, &w, &mailbox, &gateway_sequence, 0x45, 0, 0) == IPC_OK && mailbox.cmd.payload[0] == 0 && mailbox.cmd.dlc == 0, "no data: zeros");

    /* a frame that was never sealed follows the switch of the build */
    world_init(&w);
    memset((void *)&mailbox, 0, sizeof mailbox);
    mailbox.cmd.frame_type = 0x77;
    w.taken[IPC_HSEM_CMD_ID] = 1;
    {
        const IpcStatus s = Ipc_TakeCommand(&OPS, &w, &mailbox, &got);
#if HYDRA_IPC_REQUIRE_CRC
        CHECK(s == IPC_DAMAGED, "an unsealed command is refused when the build requires the seal");
#else
        CHECK(s == IPC_OK && got.frame_type == 0x77, "an unsealed command is accepted when the build does not require the seal");
#endif
    }

    if (failures == 0) printf("IPC_LINK=PASS\n");
    return failures == 0 ? 0 : 1;
}
