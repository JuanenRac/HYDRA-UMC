/* =============================================================================
 * HYDRA-UMC Firmware - tests/test_gateway_self.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Host test of the `self` callback of the gateway of the CM7 (src/mcu_stm32h757/Common/gateway_self.h): the query of the error counters is answered with type 0x0C and the two
 * counters (the receive counter held to 7 bits), a counter that cannot be read and any other frame type are left without an answer and counted, a missing callback is refused,
 * and the callback works as the `self` of a Gateway_t (the frame of the host goes in and the staged answer comes out). Evidence: simulator.
 *
 *   cc -std=c11 -Wall -Wextra -Isrc/mcu_stm32h757/Common -o test_gateway_self tests/test_gateway_self.c
 */
#include <stdio.h>
#include <string.h>

#include "gateway_self.h"

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } } while (0)

static int readable = 1;
static uint8_t tec_now = 7u, rec_now = 3u;
static int seen_ctx_ok;

static int counters(void *ctx, uint8_t *tec, uint8_t *rec)
{
    seen_ctx_ok = ctx != NULL;
    if (!readable) return 0;
    *tec = tec_now;
    *rec = rec_now;
    return 1;
}

static int no_stack(void *ctx, uint8_t slot, uint8_t type, const uint8_t *payload, uint8_t dlc, GatewayReply *reply)
{
    (void)ctx; (void)slot; (void)type; (void)payload; (void)dlc; (void)reply;
    return 0;
}

int main(void)
{
    GatewaySelf_t self;
    GatewayReply reply;
    const uint8_t none[8] = {0};
    int marker = 0;

    CHECK(GatewaySelf_Init(&self, NULL, &marker) == 0, "no callback is refused");
    CHECK(GatewaySelf_Init(&self, counters, &marker) == 1, "a good set-up");

    memset(&reply, 0xAA, sizeof reply);
    CHECK(GatewaySelf_Handle(&self, 0x0Bu, none, 0u, &reply) == 1, "the query is answered");
    CHECK(reply.type == 0x0Cu && reply.dlc == 2u && reply.payload[0] == 7u && reply.payload[1] == 3u && reply.payload[2] == 0u && seen_ctx_ok, "type 0x0C with the two counters");

    rec_now = 0xFFu;
    CHECK(GatewaySelf_Handle(&self, 0x0Bu, none, 8u, &reply) == 1 && reply.payload[1] == 0x7Fu, "the receive counter is held to 7 bits");

    readable = 0;
    CHECK(GatewaySelf_Handle(&self, 0x0Bu, none, 0u, &reply) == 0, "unreadable counters are no answer");
    readable = 1;
    CHECK(GatewaySelf_Handle(&self, 0x08u, none, 0u, &reply) == 0 && GatewaySelf_Handle(&self, 0x0Cu, none, 0u, &reply) == 0, "any other type is not answered");
    CHECK(self.answered == 2u && self.unavailable == 1u && self.ignored == 2u, "the counts");

    /* as the `self` of the gateway: the staged answer is tier 0, slot 0, type 0x0C */
    IpcOps ipc_ops;
    IpcMailbox_t mailbox;
    memset(&ipc_ops, 0, sizeof ipc_ops);
    memset(&mailbox, 0, sizeof mailbox);
    const GatewayOps ops = {GatewaySelf_Handle, no_stack};
    Gateway_t gateway;
    CHECK(Gateway_Init(&gateway, &ops, &self, &ipc_ops, NULL, &mailbox) == 1, "the gateway takes the callback");
    uint8_t request[12] = {GW_TARGET_SELF, 0u, 0x0Bu, 0u};
    uint8_t answer[GW_FRAME_BYTES];
    rec_now = 5u;
    CHECK(Gateway_Dispatch(&gateway, request, answer) == 1, "the host gets an answer");
    CHECK(answer[0] == GW_TARGET_SELF && answer[1] == 0u && answer[2] == 0x0Cu && answer[3] == 2u && answer[4] == 7u && answer[5] == 5u, "the staged frame");
    request[2] = 0x44u;
    CHECK(Gateway_Dispatch(&gateway, request, answer) == 0 && gateway.counters.self_silent == 1u, "an unknown type is silent");

    if (failures) {
        printf("gateway_self host tests: %d FAILED\n", failures);
        return 1;
    }
    printf("gateway_self host logic tests passed\n");
    return 0;
}
