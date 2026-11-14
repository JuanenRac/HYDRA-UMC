/* =============================================================================
 * HYDRA-UMC Firmware - host test of gateway_dispatch.h (what the core that owns the host link does with a frame of the host)
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * The mailbox is the double of test_ipc_link.c: four semaphores taken by ONE core and released only by it. The backend (the other core) is a function that runs while the gateway
 * waits (in the refresh callback, as the watchdog refresh does in the firmware): it takes the command and answers, stays silent, or damages its answer. The core's own handlers and
 * the stack are doubles that answer, stay silent or record what they got. HostLink_Take is run with Gateway_Dispatch as the dispatch to show that the two fit together.
 * Nothing here touches a semaphore, shared memory or a bus.
 */
#include <stdio.h>
#include <string.h>
#include "gateway_dispatch.h"
#include "host_link_master.h"

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } } while (0)

enum { GATEWAY = 0, BACKEND = 1 };
enum { BACKEND_ANSWERS = 0, BACKEND_SILENT = 1, BACKEND_DAMAGES = 2 };

typedef struct {
    int owner[IPC_HSEM_CHANNELS];
    uint32_t now;
    int backend_mode;
    unsigned commands_seen;
    IpcMessage last_command;
    uint8_t backend_sequence;
    int take_fails;
} World;

typedef struct { World *world; int core; } Core;

static IpcMailbox_t mailbox;
static Core backend_core;

static uint32_t op_tick(void *ctx) { return ((Core *)ctx)->world->now++; }
static int op_taken(void *ctx, uint32_t channel) { return ((Core *)ctx)->world->owner[channel] != -1; }
static int op_take(void *ctx, uint32_t channel)
{
    Core *c = (Core *)ctx;
    if (c->world->take_fails || c->world->owner[channel] != -1) return 0;
    c->world->owner[channel] = c->core;
    return 1;
}
static void op_release(void *ctx, uint32_t channel)
{
    Core *c = (Core *)ctx;
    if (c->world->owner[channel] == c->core) c->world->owner[channel] = -1;
}
static void op_barrier(void *ctx) { (void)ctx; }

static void op_refresh(void *ctx);
static const IpcOps OPS = {op_tick, op_refresh, op_taken, op_take, op_release, op_barrier};

/* the other core's work, run each time the gateway refreshes the watchdog while it waits */
static void op_refresh(void *ctx)
{
    World *w = ((Core *)ctx)->world;
    IpcMessage command;
    if (Ipc_TakeCommand(&OPS, &backend_core, &mailbox, &command) != IPC_OK) return;
    w->commands_seen++;
    w->last_command = command;
    if (w->backend_mode == BACKEND_SILENT) return;
    const uint8_t answer[3] = {0xA1, command.frame_type, command.dlc};
    (void)Ipc_PostResponse(&OPS, &backend_core, &mailbox, &w->backend_sequence, 0x09, answer, 3);
    if (w->backend_mode == BACKEND_DAMAGES) mailbox.resp.payload[1] ^= 0x40;
}

typedef struct {
    int self_reply;
    int stack_reply;
    unsigned self_calls, stack_calls;
    uint8_t self_type, self_dlc, self_payload[8];
    uint8_t stack_slot, stack_type, stack_dlc;
} Doubles;

static int on_self(void *ctx, uint8_t type, const uint8_t *payload, uint8_t dlc, GatewayReply *reply)
{
    Doubles *d = (Doubles *)ctx;
    d->self_calls++;
    d->self_type = type;
    d->self_dlc = dlc;
    memcpy(d->self_payload, payload, 8);
    if (!d->self_reply) return 0;
    reply->type = 0x09;
    reply->dlc = 2;
    reply->payload[0] = 0x11;
    reply->payload[1] = 0x22;
    return 1;
}

static int on_stack(void *ctx, uint8_t slot, uint8_t type, const uint8_t *payload, uint8_t dlc, GatewayReply *reply)
{
    Doubles *d = (Doubles *)ctx;
    (void)payload;
    d->stack_calls++;
    d->stack_slot = slot;
    d->stack_type = type;
    d->stack_dlc = dlc;
    if (!d->stack_reply) return 0;
    reply->type = 0x03;
    reply->dlc = 4;
    reply->payload[0] = 0xAB;
    return 1;
}

static void world_init(World *w)
{
    memset(w, 0, sizeof *w);
    for (unsigned i = 0; i < IPC_HSEM_CHANNELS; i++) w->owner[i] = -1;
    memset((void *)&mailbox, 0, sizeof mailbox);
}

static void request(uint8_t *wire, uint8_t tier, uint8_t slot, uint8_t type, uint8_t dlc, const uint8_t *payload)
{
    memset(wire, 0, GW_FRAME_BYTES);
    wire[0] = tier;
    wire[1] = slot;
    wire[2] = type;
    wire[3] = dlc;
    if (payload) memcpy(wire + 4, payload, dlc > 8 ? 8 : dlc);
}

/* the dispatch of HostLinkOps over a Gateway_t */
static int link_dispatch(void *ctx, const uint8_t *wire, uint8_t *answer12) { return Gateway_Dispatch((Gateway_t *)ctx, wire, answer12); }
static int link_transfer(void *ctx, const uint8_t *tx, uint8_t *rx)
{
    (void)tx;
    memcpy(rx, ctx, HOST_LINK_FRAME_BYTES);
    return 1;
}

int main(void)
{
    World w;
    Core gw = {&w, GATEWAY};
    Doubles d;
    Gateway_t g;
    GatewayOps ops = {on_self, on_stack};
    uint8_t wire[GW_FRAME_BYTES], answer[GW_FRAME_BYTES];
    const uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    backend_core.world = &w;
    backend_core.core = BACKEND;

    CHECK(GW_TARGET_SELF == 0u && GW_TARGET_OTHER == 1u && GW_TARGET_STACKA == 2u, "the tiers are the numbers on the wire");
    world_init(&w);
    memset(&d, 0, sizeof d);
    CHECK(Gateway_Init(&g, &ops, &d, &OPS, &gw, &mailbox) == 1, "init");
    CHECK(Gateway_Init(&g, &(GatewayOps){0, on_stack}, &d, &OPS, &gw, &mailbox) == 0, "init refuses a missing self handler");
    CHECK(Gateway_Init(&g, &(GatewayOps){on_self, 0}, &d, &OPS, &gw, &mailbox) == 0, "init refuses a missing stack handler");
    CHECK(Gateway_Init(&g, &ops, &d, 0, &gw, &mailbox) == 0, "init refuses a missing mailbox table");
    CHECK(Gateway_Init(&g, &ops, &d, &OPS, &gw, 0) == 0, "init refuses a missing mailbox");
    CHECK(Gateway_Init(&g, &ops, &d, &OPS, &gw, &mailbox) == 1, "init again");

    /* SELF: the handler gets the frame, its reply goes back with the tier SELF; no reply means no answer */
    d.self_reply = 1;
    request(wire, GW_TARGET_SELF, 0, 0x08, 8, payload);
    CHECK(Gateway_Dispatch(&g, wire, answer) == 1, "self: answered");
    CHECK(d.self_calls == 1 && d.self_type == 0x08 && d.self_dlc == 8 && memcmp(d.self_payload, payload, 8) == 0, "self: the handler got the frame");
    CHECK(answer[0] == GW_TARGET_SELF && answer[2] == 0x09 && answer[3] == 2 && answer[4] == 0x11 && answer[5] == 0x22 && answer[6] == 0, "self: the answer");
    d.self_reply = 0;
    CHECK(Gateway_Dispatch(&g, wire, answer) == 0 && g.counters.self_silent == 1, "self: no reply, no answer");

    /* a length above 8 is clamped */
    d.self_reply = 1;
    request(wire, GW_TARGET_SELF, 0, 0x02, 8, payload);
    wire[3] = 200;
    CHECK(Gateway_Dispatch(&g, wire, answer) == 1 && d.self_dlc == 8, "a length above 8 is clamped to 8");

    /* OTHER: through the mailbox, the answer of the other core goes back with the tier OTHER */
    world_init(&w);
    w.backend_mode = BACKEND_ANSWERS;
    request(wire, GW_TARGET_OTHER, 0, 0x01, 8, payload);
    const unsigned self_calls_before_other = d.self_calls;
    CHECK(Gateway_Dispatch(&g, wire, answer) == 1, "other: answered");
    CHECK(w.commands_seen == 1 && w.last_command.frame_type == 0x01 && w.last_command.dlc == 8 && memcmp(w.last_command.payload, payload, 8) == 0, "other: the backend got the command");
    CHECK(answer[0] == GW_TARGET_OTHER && answer[2] == 0x09 && answer[3] == 3 && answer[4] == 0xA1 && answer[5] == 0x01 && answer[6] == 8, "other: the answer of the backend");
    CHECK(g.counters.to_other == 1 && g.counters.other_errors == 0, "other: counted");
    CHECK(d.self_calls == self_calls_before_other && d.stack_calls == 0, "other: no other handler ran");

    /* OTHER, backend silent: the host gets STATUS_ERROR, not silence */
    world_init(&w);
    w.backend_mode = BACKEND_SILENT;
    CHECK(Gateway_Dispatch(&g, wire, answer) == 1, "silent backend: still answered");
    CHECK(answer[0] == GW_TARGET_OTHER && answer[2] == GW_FRAME_STATUS && answer[3] == 1 && answer[4] == GW_STATUS_ERROR, "silent backend: STATUS_ERROR");
    CHECK(g.counters.other_errors == 1, "silent backend: counted");

    /* OTHER, backend answers with a damaged frame: STATUS_ERROR */
    world_init(&w);
    w.backend_mode = BACKEND_DAMAGES;
    CHECK(Gateway_Dispatch(&g, wire, answer) == 1 && answer[2] == GW_FRAME_STATUS && answer[4] == GW_STATUS_ERROR, "damaged answer: STATUS_ERROR");
    CHECK(g.counters.other_errors == 2, "damaged answer: counted");

    /* OTHER, the channel refuses the post (the hardware semaphore could not be taken): STATUS_ERROR and the frame was not answered by anyone */
    world_init(&w);
    w.take_fails = 1;
    w.backend_mode = BACKEND_ANSWERS;
    CHECK(Gateway_Dispatch(&g, wire, answer) == 1 && answer[2] == GW_FRAME_STATUS && answer[4] == GW_STATUS_ERROR && w.commands_seen == 0, "refused post: STATUS_ERROR and no command reached the backend");
    CHECK(g.counters.other_errors == 3, "refused post: counted");

    /* STACK A: the slot goes to the stack, its reply goes back with the tier and the slot */
    d.stack_reply = 1;
    request(wire, GW_TARGET_STACKA, 5, 0x0E, 1, payload);
    CHECK(Gateway_Dispatch(&g, wire, answer) == 1, "stack: answered");
    CHECK(d.stack_calls == 1 && d.stack_slot == 5 && d.stack_type == 0x0E && d.stack_dlc == 1, "stack: the handler got the slot and the frame");
    CHECK(answer[0] == GW_TARGET_STACKA && answer[1] == 5 && answer[2] == 0x03 && answer[3] == 4 && answer[4] == 0xAB, "stack: the answer");
    d.stack_reply = 0;
    CHECK(Gateway_Dispatch(&g, wire, answer) == 1 && answer[0] == GW_TARGET_STACKA && answer[1] == 5 && answer[2] == GW_FRAME_STATUS && answer[4] == GW_STATUS_ERROR, "stack: no answer is STATUS_ERROR");
    CHECK(g.counters.to_stack_a == 2 && g.counters.stack_a_errors == 1, "stack: counted");

    /* an unknown tier is not run and not answered */
    const unsigned self_before = d.self_calls, stack_before = d.stack_calls;
    request(wire, 3, 0, 0x01, 0, 0);
    CHECK(Gateway_Dispatch(&g, wire, answer) == 0 && g.counters.unknown_tier == 1, "unknown tier: no answer, counted");
    request(wire, 0xFF, 0, 0x01, 0, 0);
    CHECK(Gateway_Dispatch(&g, wire, answer) == 0 && g.counters.unknown_tier == 2 && d.self_calls == self_before && d.stack_calls == stack_before, "tier 0xFF: nothing ran");

    /* the answer is exactly what HostLink_Stage takes: pack, clamp and zero the rest */
    Gateway_Pack(answer, 1, 2, 3, payload, 20);
    CHECK(answer[3] == 8 && memcmp(answer + 4, payload, 8) == 0, "pack clamps the length to 8");
    Gateway_Pack(answer, 1, 2, 3, 0, 0);
    CHECK(answer[3] == 0 && answer[4] == 0 && answer[11] == 0, "pack with no payload leaves zeros");

    /* it fits the link: a sealed frame of the host with a request for the other core is dispatched, the answer is staged */
    world_init(&w);
    w.backend_mode = BACKEND_ANSWERS;
    d.self_reply = 1;
    HostLink_t link;
    HostLinkConfig cfg = {100, 0, 3, 5, 1, 0};
    uint8_t host_wire[HOST_LINK_FRAME_BYTES];
    HostLinkOps link_ops = {link_transfer, 0, link_dispatch};
    CHECK(HostLink_Init(&link, &cfg, &link_ops, &g) == 1, "link init");
    memset(host_wire, 0xFF, sizeof host_wire);
    host_wire[0] = GW_TARGET_OTHER;
    host_wire[1] = 0;
    host_wire[2] = 0x08;
    host_wire[3] = 0;
    memset(host_wire + 4, 0, 8);
    SpiWire_Seal(host_wire, 1);
    CHECK(HostLink_Take(&link, host_wire) == HOST_LINK_RX_DISPATCH, "link: the frame is dispatched");
    CHECK(link.staged_valid == 1 && link.staged[0] == GW_TARGET_OTHER && link.staged[2] == 0x09, "link: the answer of the other core is staged for the host");

    if (failures == 0) printf("gateway_dispatch: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
