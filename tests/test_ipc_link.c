/* =============================================================================
 * HYDRA-UMC Firmware - host test of ipc_link.h (the operations on the mailbox between the two cores, for whichever core owns the host link)
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * The semaphores of the double behave like the hardware ones: a semaphore is taken by ONE core, and only the core that took it can release it (a release from the other core changes
 * nothing and is counted). Each core has its own table of callbacks over the same world. The gateway posts a command and waits for the answer, the backend takes the command and posts
 * the answer. The ORDER of the steps is logged (the frame is written before the barrier, the barrier before the semaphore is taken). The old protocol, where the reader released the
 * semaphore the writer had taken, is run against the same double to show that it cannot work. Built for both platforms and with HYDRA_IPC_REQUIRE_CRC off and on.
 * Nothing here touches a semaphore or shared memory.
 */
#include <stdio.h>
#include <string.h>
#include "ipc_link.h"

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } } while (0)

enum { GATEWAY = 0, BACKEND = 1 };

typedef struct {
    int owner[IPC_HSEM_CHANNELS];     /* -1 free, else the core that took it */
    uint32_t now;
    unsigned refreshes;
    unsigned foreign_releases;        /* releases attempted by a core that did not take the semaphore */
    char log[96];
    unsigned log_length;
    int take_fails;                   /* the next takes fail (the hardware refused) */
} World;

typedef struct { World *world; int core; } Core;

static void note(World *w, char c) { if (w->log_length < sizeof w->log - 1u) w->log[w->log_length++] = c; }
static uint32_t op_tick(void *ctx) { return ((Core *)ctx)->world->now++; }
static void op_refresh(void *ctx) { ((Core *)ctx)->world->refreshes++; }
static int op_taken(void *ctx, uint32_t channel) { return ((Core *)ctx)->world->owner[channel] != -1; }
static int op_take(void *ctx, uint32_t channel)
{
    Core *c = (Core *)ctx;
    if (c->world->take_fails || c->world->owner[channel] != -1) return 0;
    c->world->owner[channel] = c->core;
    note(c->world, 'T');
    return 1;
}
static void op_release(void *ctx, uint32_t channel)
{
    Core *c = (Core *)ctx;
    if (c->world->owner[channel] != c->core) { c->world->foreign_releases++; return; }   /* the hardware ignores it */
    c->world->owner[channel] = -1;
    note(c->world, 'R');
}
static void op_barrier(void *ctx) { note(((Core *)ctx)->world, 'B'); }

static const IpcOps OPS = {op_tick, op_refresh, op_taken, op_take, op_release, op_barrier};

static void world_init(World *w) { memset(w, 0, sizeof *w); for (unsigned i = 0; i < IPC_HSEM_CHANNELS; i++) w->owner[i] = -1; }
static int all_free(const World *w) { for (unsigned i = 0; i < IPC_HSEM_CHANNELS; i++) if (w->owner[i] != -1) return 0; return 1; }

static IpcMailbox_t mailbox;

/* both cores poll until the handshake is finished */
static void settle(World *w, Core *gw, Core *be)
{
    for (int i = 0; i < 4; i++) { Ipc_Pump(&OPS, gw, IPC_ROLE_GATEWAY); Ipc_Pump(&OPS, be, IPC_ROLE_BACKEND); }
    (void)w;
}

static unsigned lcg(unsigned *state)
{
    *state = *state * 1664525u + 1013904223u;
    return *state >> 16;
}

int main(void)
{
    World w;
    Core gw = {&w, GATEWAY}, be = {&w, BACKEND};
    IpcMessage got;
    uint8_t gateway_sequence = 0, backend_sequence = 0;
    const uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};

#if defined(HYDRA_PLATFORM_H757)
    CHECK(IPC_GATEWAY_CORE == HYDRA_OWNER_CM7 && IPC_BACKEND_CORE == HYDRA_OWNER_CM4, "on the H757 platform the CM7 is the gateway");
#else
    CHECK(IPC_GATEWAY_CORE == HYDRA_OWNER_CM4 && IPC_BACKEND_CORE == HYDRA_OWNER_CM7, "on the H745 platform the CM4 is the gateway");
#endif
    /* four different semaphores for the four roles; none is the one of the clock-tree handshake */
    CHECK(IPC_HSEM_CMD_ID != IPC_HSEM_RESP_ID && IPC_HSEM_CMD_ID != IPC_HSEM_CMD_DONE_ID && IPC_HSEM_CMD_ID != IPC_HSEM_RESP_DONE_ID
          && IPC_HSEM_RESP_ID != IPC_HSEM_CMD_DONE_ID && IPC_HSEM_RESP_ID != IPC_HSEM_RESP_DONE_ID && IPC_HSEM_CMD_DONE_ID != IPC_HSEM_RESP_DONE_ID, "the four mailbox semaphores are different");

    /* a round trip: command, answer; each core released only what it took; the channels are free again */
    world_init(&w);
    memset((void *)&mailbox, 0, sizeof mailbox);
    CHECK(Ipc_PostCommand(&OPS, &gw, &mailbox, &gateway_sequence, 0x05, payload, 8) == IPC_OK, "post the command");
    CHECK(w.owner[IPC_HSEM_CMD_ID] == GATEWAY, "the gateway took the posted semaphore");
    CHECK(Ipc_TakeCommand(&OPS, &be, &mailbox, &got) == IPC_OK && got.frame_type == 0x05 && got.dlc == 8 && memcmp(got.payload, payload, 8) == 0, "the backend takes the command as it was posted");
    CHECK(w.owner[IPC_HSEM_CMD_DONE_ID] == BACKEND, "the backend acknowledged with its own semaphore");
    CHECK(Ipc_TakeCommand(&OPS, &be, &mailbox, &got) == IPC_NONE, "the same command is not taken twice");
    const uint8_t answer[2] = {0x10, 0x20};
    CHECK(Ipc_PostResponse(&OPS, &be, &mailbox, &backend_sequence, 0x0F, answer, 2) == IPC_OK, "post the answer");
    CHECK(Ipc_WaitResponse(&OPS, &gw, &mailbox, &got) == IPC_OK && got.frame_type == 0x0F && got.dlc == 2 && got.payload[0] == 0x10 && got.payload[1] == 0x20 && got.payload[2] == 0, "the gateway gets the answer, padded with zeros");
    settle(&w, &gw, &be);
    CHECK(all_free(&w), "all four semaphores are free after the exchange");
    CHECK(w.foreign_releases == 0u, "no core tried to release what the other took");
    CHECK(gateway_sequence == 1 && backend_sequence == 1, "each sequence number rose by one");
    CHECK(mailbox.cmd._reserved[0] == 0 && mailbox.resp._reserved[0] == 0, "the sequence number sits in the frame");

    /* the order of the steps of a post: barrier before the take */
    world_init(&w);
    memset((void *)&mailbox, 0, sizeof mailbox);
    CHECK(Ipc_PostCommand(&OPS, &gw, &mailbox, &gateway_sequence, 0x05, payload, 8) == IPC_OK, "post");
    CHECK(strcmp(w.log, "BT") == 0, "the barrier comes before the semaphore is taken");

    /* a second command while the first is not finished: busy, and the frame is NOT touched */
    world_init(&w);
    memset((void *)&mailbox, 0, sizeof mailbox);
    CHECK(Ipc_PostCommand(&OPS, &gw, &mailbox, &gateway_sequence, 0x11, payload, 8) == IPC_OK, "first command");
    const uint8_t second[8] = {9, 9, 9, 9, 9, 9, 9, 9};
    const uint8_t seq_before = gateway_sequence;
    CHECK(Ipc_PostCommand(&OPS, &gw, &mailbox, &gateway_sequence, 0x22, second, 8) == IPC_BUSY, "second command: busy");
    CHECK(mailbox.cmd.frame_type == 0x11 && mailbox.cmd.payload[0] == 1 && gateway_sequence == seq_before, "the frame and the sequence are untouched");
    CHECK(w.refreshes > 0u && w.now >= IPC_POST_COMMAND_WAIT_MS, "the watchdog was refreshed and it waited for the time");

    /* a command read but not yet finished by the backend: the next post waits for the backend to let go, and goes when it has */
    world_init(&w);
    memset((void *)&mailbox, 0, sizeof mailbox);
    CHECK(Ipc_PostCommand(&OPS, &gw, &mailbox, &gateway_sequence, 0x31, payload, 8) == IPC_OK, "post");
    CHECK(Ipc_TakeCommand(&OPS, &be, &mailbox, &got) == IPC_OK, "taken");
    CHECK(Ipc_PostCommand(&OPS, &gw, &mailbox, &gateway_sequence, 0x32, payload, 8) == IPC_BUSY, "the backend has not finished its half: busy");
    Ipc_Pump(&OPS, &be, IPC_ROLE_BACKEND);   /* the gateway released POSTED in its loop above; the backend releases DONE when it polls */
    CHECK(Ipc_PostCommand(&OPS, &gw, &mailbox, &gateway_sequence, 0x32, payload, 8) == IPC_OK, "after the backend polled the next command goes");
    CHECK(w.foreign_releases == 0u, "still no foreign release");

    /* a command damaged in the shared memory is reported and dropped, and the handshake still finishes */
    world_init(&w);
    memset((void *)&mailbox, 0, sizeof mailbox);
    CHECK(Ipc_PostCommand(&OPS, &gw, &mailbox, &gateway_sequence, 0x33, payload, 8) == IPC_OK, "post");
    mailbox.cmd.payload[3] ^= 0x40;
    CHECK(Ipc_TakeCommand(&OPS, &be, &mailbox, &got) == IPC_DAMAGED, "a damaged command is reported");
    settle(&w, &gw, &be);
    CHECK(all_free(&w), "the channel is free again after a damaged command");

    /* nothing posted: nothing taken */
    world_init(&w);
    CHECK(Ipc_TakeCommand(&OPS, &be, &mailbox, &got) == IPC_NONE, "no command: none");

    /* no answer: time-out, after the time */
    world_init(&w);
    CHECK(Ipc_WaitResponse(&OPS, &gw, &mailbox, &got) == IPC_TIMEOUT && w.now >= IPC_WAIT_RESPONSE_MS && w.refreshes > 0u, "no answer: time-out, with the watchdog refreshed");

    /* a damaged answer is reported, and the handshake finishes */
    world_init(&w);
    memset((void *)&mailbox, 0, sizeof mailbox);
    CHECK(Ipc_PostResponse(&OPS, &be, &mailbox, &backend_sequence, 0x0F, answer, 2) == IPC_OK, "post the answer");
    mailbox.resp.dlc ^= 0x01;
    CHECK(Ipc_WaitResponse(&OPS, &gw, &mailbox, &got) == IPC_DAMAGED, "a damaged answer is reported");
    settle(&w, &gw, &be);
    CHECK(all_free(&w), "the channel is free again after a damaged answer");

    /* the gateway does not drain the answer: the backend drops the next one instead of blocking, and does not touch the frame */
    world_init(&w);
    memset((void *)&mailbox, 0, sizeof mailbox);
    CHECK(Ipc_PostResponse(&OPS, &be, &mailbox, &backend_sequence, 0x0A, answer, 2) == IPC_OK, "first answer");
    CHECK(Ipc_PostResponse(&OPS, &be, &mailbox, &backend_sequence, 0x0B, answer, 2) == IPC_BUSY && mailbox.resp.frame_type == 0x0A, "second answer: dropped, the first is intact");

    /* a semaphore the hardware refuses to give is a busy channel, not a half-posted frame left as if it were posted */
    world_init(&w);
    memset((void *)&mailbox, 0, sizeof mailbox);
    w.take_fails = 1;
    CHECK(Ipc_PostCommand(&OPS, &gw, &mailbox, &gateway_sequence, 0x44, payload, 8) == IPC_BUSY && w.owner[IPC_HSEM_CMD_ID] == -1, "a refused take is reported as busy");

    /* a length beyond 8 is clamped to the payload and the rest is zero */
    world_init(&w);
    memset((void *)&mailbox, 0, sizeof mailbox);
    CHECK(Ipc_PostCommand(&OPS, &gw, &mailbox, &gateway_sequence, 0x44, payload, 200) == IPC_OK && mailbox.cmd.payload[7] == 8, "a long length is clamped to the 8 bytes of payload");
    memset((void *)&mailbox, 0, sizeof mailbox);
    world_init(&w);
    CHECK(Ipc_PostCommand(&OPS, &gw, &mailbox, &gateway_sequence, 0x45, 0, 0) == IPC_OK && mailbox.cmd.payload[0] == 0 && mailbox.cmd.dlc == 0, "no data: zeros");

    /* a frame that was never sealed follows the switch of the build */
    world_init(&w);
    memset((void *)&mailbox, 0, sizeof mailbox);
    mailbox.cmd.frame_type = 0x77;
    w.owner[IPC_HSEM_CMD_ID] = GATEWAY;
    {
        const IpcStatus s = Ipc_TakeCommand(&OPS, &be, &mailbox, &got);
#if HYDRA_IPC_REQUIRE_CRC
        CHECK(s == IPC_DAMAGED, "an unsealed command is refused when the build requires the seal");
#else
        CHECK(s == IPC_OK && got.frame_type == 0x77, "an unsealed command is accepted when the build does not require the seal");
#endif
    }

    /* the OLD protocol, run against the same double: the reader releases the semaphore the writer took. The hardware ignores it, the channel stays taken for ever and no second frame is ever sent. */
    world_init(&w);
    memset((void *)&mailbox, 0, sizeof mailbox);
    CHECK(op_take(&gw, IPC_HSEM_CMD_ID) == 1, "old protocol: the writer takes the semaphore");
    op_release(&be, IPC_HSEM_CMD_ID);   /* the reader 'frees the slot' */
    CHECK(w.owner[IPC_HSEM_CMD_ID] == GATEWAY && w.foreign_releases == 1u, "old protocol: the release of the other core changes nothing");

    /* many random interleavings of the two cores through the real operations: every command and every answer arrives once, in order, with no foreign release */
    world_init(&w);
    memset((void *)&mailbox, 0, sizeof mailbox);
    unsigned state = 4242u;
    uint8_t next_cmd = 0, expect_cmd = 0, next_resp = 0, expect_resp = 0;
    unsigned cmds = 0, resps = 0;
    int pending_answer = 0;
    for (unsigned step = 0; step < 40000u; step++) {
        const unsigned pick = lcg(&state) % 6u;
        if (pick == 0u) {
            if (Ipc_PostCommand(&OPS, &gw, &mailbox, &gateway_sequence, next_cmd, &next_cmd, 1) == IPC_OK) { next_cmd++; cmds++; }
        } else if (pick == 1u || pick == 2u) {
            if (!pending_answer && Ipc_TakeCommand(&OPS, &be, &mailbox, &got) == IPC_OK) {
                CHECK(got.frame_type == expect_cmd && got.payload[0] == expect_cmd, "commands keep their order");
                expect_cmd++;
                pending_answer = 1;
            }
        } else if (pick == 3u) {
            if (pending_answer && Ipc_PostResponse(&OPS, &be, &mailbox, &backend_sequence, next_resp, &next_resp, 1) == IPC_OK) { next_resp++; pending_answer = 0; }
        } else if (pick == 4u) {
            if (Ipc_TakeResponse(&OPS, &gw, &mailbox, &got) == IPC_OK) {
                CHECK(got.frame_type == expect_resp && got.payload[0] == expect_resp, "answers keep their order");
                expect_resp++;
                resps++;
            }
        } else {
            Ipc_Pump(&OPS, &gw, IPC_ROLE_GATEWAY);
            Ipc_Pump(&OPS, &be, IPC_ROLE_BACKEND);
        }
    }
    CHECK(cmds > 500u && resps > 500u, "many commands and answers went through");
    CHECK(w.foreign_releases == 0u, "in all those interleavings no core released what the other took");
    CHECK((uint8_t)((uint8_t)cmds - expect_cmd) <= 1u, "no command was lost or repeated (at most one is in flight)");

    if (failures == 0) printf("IPC_LINK=PASS\n");
    return failures == 0 ? 0 : 1;
}
