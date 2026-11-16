/* =============================================================================
 * HYDRA-UMC Firmware - tests/test_gateway_stack_a.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Host test of gateway_stack_a.h (the `stack_a` callback of the gateway of the CM7, over FDCAN1) against a double of the bus: a queue of received frames, a log of the frames sent
 * and a clock that moves a millisecond with every look. It checks the id of a frame (0x600 + slot * 0x40 + type), the refusals (slot over 7, a type outside the window, the types
 * that the boards transmit themselves), the poll type that sends nothing, the wait that drops the frame of another window and takes the first one of the right window, the time-out,
 * a frame that could not be queued, and the fit with Gateway_Dispatch. Evidence: simulator.
 */
#include <stdio.h>
#include <string.h>

#include "gateway_stack_a.h"
#include "ipc_link.h"

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } } while (0)

typedef struct {
    uint32_t id;
    uint8_t data[8];
    uint8_t dlc;
} Frame;

typedef struct {
    uint32_t now;
    Frame inbox[8];
    unsigned inbox_count, inbox_read;
    Frame sent[8];
    unsigned sent_count;
    int tx_accepts;
    unsigned kicks;
} Bus;

static int bus_tx(void *ctx, uint32_t id, const uint8_t *data, uint8_t dlc)
{
    Bus *b = (Bus *)ctx;
    if (!b->tx_accepts) return 0;
    Frame *f = &b->sent[b->sent_count++ % 8u];
    f->id = id;
    f->dlc = dlc;
    memcpy(f->data, data, 8);
    return 1;
}

static int bus_rx(void *ctx, uint32_t *id, uint8_t *data, uint8_t *dlc)
{
    Bus *b = (Bus *)ctx;
    if (b->inbox_read >= b->inbox_count) return 0;
    const Frame *f = &b->inbox[b->inbox_read++];
    *id = f->id;
    *dlc = f->dlc;
    memcpy(data, f->data, 8);
    return 1;
}

static uint32_t bus_now(void *ctx) { return ((Bus *)ctx)->now++; }
static void bus_kick(void *ctx) { ((Bus *)ctx)->kicks++; }

static void queue(Bus *b, uint32_t id, uint8_t dlc, uint8_t first)
{
    Frame *f = &b->inbox[b->inbox_count++];
    memset(f, 0, sizeof *f);
    f->id = id;
    f->dlc = dlc;
    for (unsigned i = 0; i < 8u; i++) f->data[i] = (uint8_t)(first + i);
}

static Bus bus;
static GatewayStackA_t stack;
static const GatewayCanOps CAN = {bus_tx, bus_rx, bus_now, bus_kick};

static void reset(void)
{
    memset(&bus, 0, sizeof bus);
    bus.tx_accepts = 1;
    GatewayStackA_Init(&stack, &CAN, &bus, 10u);
}

static int self_handler(void *ctx, uint8_t type, const uint8_t *payload, uint8_t dlc, GatewayReply *reply)
{
    (void)ctx; (void)type; (void)payload; (void)dlc; (void)reply;
    return 0;
}

int main(void)
{
    GatewayReply reply;
    const uint8_t payload[8] = {0xA1, 0xA2, 0xA3};

    /* the start-up */
    CHECK(!GatewayStackA_Init(&stack, NULL, &bus, 10u), "no operations");
    GatewayCanOps broken = CAN;
    broken.tx = NULL;
    CHECK(!GatewayStackA_Init(&stack, &broken, &bus, 10u), "no tx");
    broken = CAN; broken.rx = NULL;
    CHECK(!GatewayStackA_Init(&stack, &broken, &bus, 10u), "no rx");
    broken = CAN; broken.now_ms = NULL;
    CHECK(!GatewayStackA_Init(&stack, &broken, &bus, 10u), "no clock");
    CHECK(!GatewayStackA_Init(&stack, &CAN, &bus, 0u), "time-out 0");
    CHECK(!GatewayStackA_Init(&stack, &CAN, &bus, 1001u), "time-out over 1000");
    CHECK(GatewayStackA_Init(&stack, &CAN, &bus, 1000u) && GatewayStackA_Init(&stack, &CAN, &bus, 1u), "the limits are accepted");

    /* a frame goes out on its id and the first frame of the window comes back */
    reset();
    queue(&bus, 0x600UL + 3UL * 0x40UL + 0x05UL, 1, 0x10); /* the answer of slot 3 */
    CHECK(GatewayStackA_Exchange(&stack, 3u, 0x01u, payload, 3u, &reply) == 1, "an exchange with a board that answers");
    CHECK(bus.sent_count == 1u && bus.sent[0].id == 0x600UL + 3UL * 0x40UL + 0x01UL && bus.sent[0].dlc == 3u, "the frame went out on the id of the slot");
    CHECK(bus.sent[0].data[0] == 0xA1u && bus.sent[0].data[2] == 0xA3u && bus.sent[0].data[3] == 0u, "payload copied, rest zero");
    CHECK(reply.type == 0x05u && reply.dlc == 1u && reply.payload[0] == 0x10u && reply.payload[7] == 0x17u, "the answer is the frame of the window");
    CHECK(stack.counters.sent == 1u && stack.counters.answered == 1u && bus.kicks > 0u, "counted, and the watchdog was kicked while waiting");

    /* a frame of another window is dropped; the right one after it is taken; a dlc over 8 is clamped */
    reset();
    queue(&bus, 0x600UL + 3UL * 0x40UL + 0x06UL, 2, 0x20);       /* heartbeat of slot 3 */
    queue(&bus, 0x600UL + 4UL * 0x40UL - 1UL, 2, 0x30);          /* the last id of slot 3 */
    queue(&bus, 0x600UL + 4UL * 0x40UL, 12, 0x40);               /* the first id of slot 4, a damaged length */
    CHECK(GatewayStackA_Exchange(&stack, 4u, 0x02u, NULL, 0u, &reply) == 1, "the exchange takes the first frame of its window");
    CHECK(reply.type == 0x00u && reply.dlc == 8u && reply.payload[0] == 0x40u, "type 0 and a dlc clamped to 8");
    CHECK(stack.counters.foreign_dropped == 2u, "the frames of the neighbour below were dropped");
    CHECK(bus.sent[0].dlc == 0u, "a frame without payload goes out empty");

    /* ... and a frame above the window is dropped too */
    reset();
    queue(&bus, 0x600UL + 5UL * 0x40UL, 1, 1);
    queue(&bus, 0x600UL + 4UL * 0x40UL + 0x3FUL, 1, 2);
    CHECK(GatewayStackA_Exchange(&stack, 4u, 0x02u, NULL, 0u, &reply) == 1 && reply.type == 0x3Fu && stack.counters.foreign_dropped == 1u, "the last id of the window is in, the next window is out");

    /* the dlc of the request is clamped to 8 */
    reset();
    queue(&bus, 0x600UL + 0x01UL, 0, 0);
    CHECK(GatewayStackA_Exchange(&stack, 0u, 0x04u, payload, 9u, &reply) == 1 && bus.sent[0].dlc == 8u, "a request dlc over 8 goes out as 8");

    /* the refusals put nothing on the bus */
    reset();
    CHECK(GatewayStackA_Exchange(&stack, 8u, 0x01u, payload, 1u, &reply) == 0, "slot 8 does not exist");
    CHECK(GatewayStackA_Exchange(&stack, 0u, 0x40u, payload, 1u, &reply) == 0, "type 0x40 is outside the window");
    CHECK(GatewayStackA_Exchange(&stack, 0u, 0xFFu, payload, 1u, &reply) == 0, "type 0xFF is outside the window");
    const uint8_t by_board[] = {0x03, 0x05, 0x06, 0x09, 0x0A, 0x0C, 0x0F, 0x11, 0x16, 0x18, 0x1C, 0x1D, 0x1F, 0x21, 0x23};
    for (unsigned i = 0; i < sizeof by_board; i++) CHECK(GatewayStackA_Exchange(&stack, 1u, by_board[i], payload, 1u, &reply) == 0, "a type that the boards transmit is not sent");
    CHECK(bus.sent_count == 0u && stack.counters.refused == 3u + sizeof by_board, "nothing was sent, all counted");
    for (unsigned t = 0; t < 0x40u; t++) {
        int in_list = 0;
        for (unsigned i = 0; i < sizeof by_board; i++) in_list |= (by_board[i] == t);
        CHECK(((GSA_TX_BY_BOARD_MASK >> t) & 1ULL) == (in_list ? 1ULL : 0ULL), "the mask is exactly the list");
    }

    /* the poll type sends nothing and waits for the next frame of the board */
    reset();
    queue(&bus, 0x600UL + 2UL * 0x40UL + 0x06UL, 2, 0x50);
    CHECK(GatewayStackA_Exchange(&stack, 2u, GSA_TYPE_POLL_STATUS, payload, 1u, &reply) == 1 && reply.type == 0x06u, "a poll takes the next frame");
    CHECK(bus.sent_count == 0u && stack.counters.listen_only == 1u && stack.counters.sent == 0u, "and puts nothing on the bus");

    /* silence ends at the time-out; with no watchdog hook it ends too */
    reset();
    CHECK(GatewayStackA_Exchange(&stack, 1u, 0x01u, payload, 1u, &reply) == 0 && stack.counters.timeouts == 1u, "no answer is a time-out");
    CHECK(bus.now >= 10u && bus.now < 40u, "the wait lasted about the time-out");
    GatewayCanOps no_kick = CAN;
    no_kick.kick = NULL;
    reset();
    GatewayStackA_Init(&stack, &no_kick, &bus, 5u);
    CHECK(GatewayStackA_Exchange(&stack, 1u, 0x01u, payload, 1u, &reply) == 0 && bus.kicks == 0u, "it ends without a watchdog hook");

    /* a frame that could not be queued is an error at once, without waiting */
    reset();
    bus.tx_accepts = 0;
    CHECK(GatewayStackA_Exchange(&stack, 1u, 0x01u, payload, 1u, &reply) == 0 && stack.counters.tx_failed == 1u && bus.now == 0u, "a refused queue entry is an error without waiting");

    /* the fit with the gateway: tier 2 goes to the callback, no answer is STATUS_ERROR */
    reset();
    static IpcMailbox_t mailbox;
    static const IpcOps IPC = {0};
    Gateway_t gw;
    const GatewayOps ops = {self_handler, GatewayStackA_Exchange};
    CHECK(Gateway_Init(&gw, &ops, &stack, &IPC, NULL, &mailbox), "the gateway starts with the callback");
    uint8_t request[12] = {GW_TARGET_STACKA, 2u, 0x01u, 1u, 0x77u};
    uint8_t answer[GW_FRAME_BYTES];
    queue(&bus, 0x600UL + 2UL * 0x40UL + 0x05UL, 1, 0x01);
    CHECK(Gateway_Dispatch(&gw, request, answer) == 1 && answer[0] == GW_TARGET_STACKA && answer[1] == 2u && answer[2] == 0x05u && answer[3] == 1u && answer[4] == 0x01u, "an answer of the stack reaches the host");
    reset();
    gw.ctx = &stack;
    CHECK(Gateway_Dispatch(&gw, request, answer) == 1 && answer[2] == GW_FRAME_STATUS && answer[4] == GW_STATUS_ERROR, "silence of the stack is STATUS_ERROR");

    if (failures) {
        printf("gateway_stack_a host tests: %d FAILED\n", failures);
        return 1;
    }
    printf("gateway_stack_a host logic tests passed\n");
    return 0;
}
