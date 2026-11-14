/* =============================================================================
 * HYDRA-UMC Firmware - src/mcu_stm32h757/Common/gateway_dispatch.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * gateway_dispatch.h - what the core that owns the host link does with a frame of the host (HAL-free, host-testable). On the H757 board the CM7 owns SPI3, FDCAN1 and I2C4, so
 * it is the gateway that the CM4 used to be: it reads the frame from the host (HostLink_Poll calls Gateway_Dispatch for each request that was not a duplicate) and sends it where
 * its target tier says, with the same three targets as the gateway of the H745 board (CM4/boot/bootloader_main.c, DispatchFrame):
 *   GW_TARGET_SELF    the gateway core's own handlers (the callback `self`), which may answer or not;
 *   GW_TARGET_OTHER   the other core, through the mailbox (Ipc_PostCommand, then Ipc_WaitResponse). On the H745 board that tier is named CM7 and the other core is the CM7; the number on
 *                     the wire is the same and means "the other core". When the post is refused or the answer does not come (or is damaged) the host gets STATUS_ERROR, never silence;
 *   GW_TARGET_STACKA  the stack of controller boards on FDCAN1, through the callback `stack_a` (the slot is in the frame); no answer from it is STATUS_ERROR too.
 * Any other tier is not run and not answered (it is counted). The length of a frame is clamped to 8, the size of a payload. The answer is the 12 bytes that HostLink_Stage takes:
 * tier, slot, frame type, length and 8 bytes of payload. The mailbox is reached only through IpcOps, the stack and the core's own handlers only through the callbacks: nothing here
 * touches the hardware. tests/test_gateway_dispatch.c runs it against a double of the mailbox with a backend that answers, stays silent or damages its answer. Nothing here has run
 * on a board.
 * ============================================================================= */
#ifndef HYDRA_UMC_GATEWAY_DISPATCH_H
#define HYDRA_UMC_GATEWAY_DISPATCH_H

#include <stdint.h>
#include <string.h>

#include "ipc_link.h"

#define GW_TARGET_SELF 0u
#define GW_TARGET_OTHER 1u
#define GW_TARGET_STACKA 2u

/* the frame types the gateway itself puts in an answer: the same numbers as OFS_STATUS and STATUS_ERROR of the bootloaders (tests/test_gateway_dispatch.py checks it) */
#define GW_FRAME_STATUS 0x05u
#define GW_STATUS_ERROR 0xFFu

#define GW_FRAME_BYTES 12u

/* A reply from a handler: the frame type, up to 8 bytes and their number. */
typedef struct {
    uint8_t type;
    uint8_t dlc;
    uint8_t payload[8];
} GatewayReply;

typedef struct {
    /* the gateway core's own handler: returns 1 when it has a reply to give, 0 when it has none */
    int (*self)(void *ctx, uint8_t type, const uint8_t *payload, uint8_t dlc, GatewayReply *reply);
    /* sends the frame to the stack in `slot` and waits for its answer: returns 1 when one came, 0 when none came */
    int (*stack_a)(void *ctx, uint8_t slot, uint8_t type, const uint8_t *payload, uint8_t dlc, GatewayReply *reply);
} GatewayOps;

typedef struct {
    uint32_t to_self, to_other, to_stack_a, unknown_tier, self_silent, other_errors, stack_a_errors;
} GatewayCounters;

typedef struct {
    GatewayOps ops;
    void *ctx;                 /* handed to the callbacks of `ops` */
    const IpcOps *ipc;         /* the operations of this core on the mailbox */
    void *ipc_ctx;
    IpcMailbox_t *mailbox;
    uint8_t cmd_sequence;
    GatewayCounters counters;
} Gateway_t;

/* Returns 0 when a callback or the mailbox is missing. */
static inline int Gateway_Init(Gateway_t *g, const GatewayOps *ops, void *ctx, const IpcOps *ipc, void *ipc_ctx, IpcMailbox_t *mailbox)
{
    if (!ops || !ops->self || !ops->stack_a || !ipc || !mailbox) return 0;
    memset(g, 0, sizeof *g);
    g->ops = *ops;
    g->ctx = ctx;
    g->ipc = ipc;
    g->ipc_ctx = ipc_ctx;
    g->mailbox = mailbox;
    return 1;
}

static inline void Gateway_Pack(uint8_t *answer12, uint8_t tier, uint8_t slot, uint8_t type, const uint8_t *payload, uint8_t dlc)
{
    const uint8_t length = dlc > 8u ? 8u : dlc;
    memset(answer12, 0, GW_FRAME_BYTES);
    answer12[0] = tier;
    answer12[1] = slot;
    answer12[2] = type;
    answer12[3] = length;
    if (payload && length) memcpy(answer12 + 4, payload, length);
}

static inline void Gateway_PackError(uint8_t *answer12, uint8_t tier, uint8_t slot)
{
    const uint8_t error = GW_STATUS_ERROR;
    Gateway_Pack(answer12, tier, slot, GW_FRAME_STATUS, &error, 1u);
}

/* `request` is the wire frame of the host (tier, slot, type, length, payload at 0, 1, 2, 3, 4..11); the answer goes to `answer12`. Returns 1 when there is an answer to stage. */
static inline int Gateway_Dispatch(Gateway_t *g, const uint8_t *request, uint8_t *answer12)
{
    const uint8_t tier = request[0];
    const uint8_t slot = request[1];
    const uint8_t type = request[2];
    const uint8_t dlc = request[3] > 8u ? 8u : request[3];
    const uint8_t *payload = request + 4;
    GatewayReply reply;
    memset(&reply, 0, sizeof reply);

    if (tier == GW_TARGET_SELF) {
        g->counters.to_self++;
        if (!g->ops.self(g->ctx, type, payload, dlc, &reply)) {
            g->counters.self_silent++;
            return 0;
        }
        Gateway_Pack(answer12, GW_TARGET_SELF, 0u, reply.type, reply.payload, reply.dlc);
        return 1;
    }
    if (tier == GW_TARGET_OTHER) {
        g->counters.to_other++;
        IpcMessage answer;
        if (Ipc_PostCommand(g->ipc, g->ipc_ctx, g->mailbox, &g->cmd_sequence, type, payload, dlc) != IPC_OK || Ipc_WaitResponse(g->ipc, g->ipc_ctx, g->mailbox, &answer) != IPC_OK) {
            g->counters.other_errors++;
            Gateway_PackError(answer12, GW_TARGET_OTHER, 0u);
            return 1;
        }
        Gateway_Pack(answer12, GW_TARGET_OTHER, 0u, answer.frame_type, answer.payload, answer.dlc);
        return 1;
    }
    if (tier == GW_TARGET_STACKA) {
        g->counters.to_stack_a++;
        if (!g->ops.stack_a(g->ctx, slot, type, payload, dlc, &reply)) {
            g->counters.stack_a_errors++;
            Gateway_PackError(answer12, GW_TARGET_STACKA, slot);
            return 1;
        }
        Gateway_Pack(answer12, GW_TARGET_STACKA, slot, reply.type, reply.payload, reply.dlc);
        return 1;
    }
    g->counters.unknown_tier++;
    return 0;
}

#endif /* HYDRA_UMC_GATEWAY_DISPATCH_H */
