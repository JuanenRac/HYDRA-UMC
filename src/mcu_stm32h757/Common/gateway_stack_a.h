/* =============================================================================
 * HYDRA-UMC Firmware - src/mcu_stm32h757/Common/gateway_stack_a.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * gateway_stack_a.h - the `stack_a` callback of the gateway of the CM7 (gateway_dispatch.h), over FDCAN1 (HAL-free, host-testable). It does what Relay_ToStackA does on the CM4
 * of the H745 board: a frame for the board in `slot` goes out on the id  0x600 + slot * 0x40 + type  and the first frame of the same window that comes back is the answer. The
 * rules are the same as the ones of the CM4: only slots 0 to 7 exist; a type outside the window of 0x40 ids is refused (it would address the next board); the types that the
 * boards themselves transmit are never put on the bus from here (one emitter per identifier); the poll type (0x17) puts nothing on the bus and only waits for the next frame of
 * the board. A frame of another window is dropped and the wait goes on; when the time is over, or the frame could not be queued, the callback returns 0 and the gateway answers
 * STATUS_ERROR. The bus is reached only through GatewayCanOps (the HAL on the board, a double in tests/test_gateway_stack_a.c). Nothing here has run on a board.
 * ============================================================================= */
#ifndef HYDRA_UMC_GATEWAY_STACK_A_H
#define HYDRA_UMC_GATEWAY_STACK_A_H

#include <stdint.h>
#include <string.h>

#include "gateway_dispatch.h"

#define GSA_BASE_ID 0x600UL
#define GSA_SLOT_WINDOW 0x40UL
#define GSA_SLOT_MAX 7u
#define GSA_TYPE_POLL_STATUS 0x17u
#define GSA_TIMEOUT_MIN_MS 1u
#define GSA_TIMEOUT_MAX_MS 1000u

/* the types that the boards transmit themselves (page ack, status, heartbeat, version, bootloader version, error counters, backup read, axis status, relay receive, downgrade nonce,
 * FRAM erase and self-test answers, axis step telemetry or health frame, stop state, slot scan reply), one bit per type: tests/test_gateway_stack_a.py compares them with the list of the CM4 bootloader */
#define GSA_TX_BY_BOARD_MASK                                                                                                                                                \
    ((1ULL << 0x03) | (1ULL << 0x05) | (1ULL << 0x06) | (1ULL << 0x09) | (1ULL << 0x0A) | (1ULL << 0x0C) | (1ULL << 0x0F) | (1ULL << 0x11) | (1ULL << 0x16) | (1ULL << 0x18) | (1ULL << 0x1C) | (1ULL << 0x1D) | (1ULL << 0x1F) | \
     (1ULL << 0x21) | (1ULL << 0x23))

typedef struct {
    /* queues one standard-id data frame; returns 1 when it was accepted */
    int (*tx)(void *ctx, uint32_t id, const uint8_t *data, uint8_t dlc);
    /* takes the next received frame if there is one: returns 1 and fills id (standard), data[8] and dlc (0..8); returns 0 when the FIFO is empty */
    int (*rx)(void *ctx, uint32_t *id, uint8_t *data, uint8_t *dlc);
    uint32_t (*now_ms)(void *ctx);
    void (*kick)(void *ctx); /* called on every turn of the wait (the watchdog of the core); may be NULL */
} GatewayCanOps;

typedef struct {
    uint32_t sent, listen_only, refused, answered, timeouts, tx_failed, foreign_dropped;
} GatewayStackACounters;

typedef struct {
    GatewayCanOps ops;
    void *can_ctx;
    uint32_t timeout_ms;
    GatewayStackACounters counters;
} GatewayStackA_t;

/* Returns 0 when a callback is missing or the time-out is outside 1..1000 ms. */
static inline int GatewayStackA_Init(GatewayStackA_t *s, const GatewayCanOps *ops, void *can_ctx, uint32_t timeout_ms)
{
    if (!ops || !ops->tx || !ops->rx || !ops->now_ms || timeout_ms < GSA_TIMEOUT_MIN_MS || timeout_ms > GSA_TIMEOUT_MAX_MS) return 0;
    memset(s, 0, sizeof *s);
    s->ops = *ops;
    s->can_ctx = can_ctx;
    s->timeout_ms = timeout_ms;
    return 1;
}

/* The `stack_a` callback of GatewayOps: `ctx` is the GatewayStackA_t. Returns 1 with the reply filled, 0 when there is no answer. */
static inline int GatewayStackA_Exchange(void *ctx, uint8_t slot, uint8_t type, const uint8_t *payload, uint8_t dlc, GatewayReply *reply)
{
    GatewayStackA_t *s = (GatewayStackA_t *)ctx;
    if (slot > GSA_SLOT_MAX || type >= GSA_SLOT_WINDOW || ((GSA_TX_BY_BOARD_MASK >> type) & 1ULL)) {
        s->counters.refused++;
        return 0;
    }
    const uint32_t base = GSA_BASE_ID + (uint32_t)slot * GSA_SLOT_WINDOW;
    if (type == GSA_TYPE_POLL_STATUS) {
        s->counters.listen_only++;
    } else {
        uint8_t data[8] = {0};
        const uint8_t length = dlc > 8u ? 8u : dlc;
        if (length) memcpy(data, payload, length);
        if (!s->ops.tx(s->can_ctx, base + type, data, length)) {
            s->counters.tx_failed++;
            return 0;
        }
        s->counters.sent++;
    }
    const uint32_t start = s->ops.now_ms(s->can_ctx);
    while ((uint32_t)(s->ops.now_ms(s->can_ctx) - start) < s->timeout_ms) {
        if (s->ops.kick) s->ops.kick(s->can_ctx);
        uint32_t id;
        uint8_t data[8];
        uint8_t length;
        if (!s->ops.rx(s->can_ctx, &id, data, &length)) continue;
        if (id < base || id >= base + GSA_SLOT_WINDOW) {
            s->counters.foreign_dropped++; /* another board's frame: not what is waited for */
            continue;
        }
        memset(reply, 0, sizeof *reply);
        reply->type = (uint8_t)(id - base);
        reply->dlc = length > 8u ? 8u : length;
        memcpy(reply->payload, data, 8);
        s->counters.answered++;
        return 1;
    }
    s->counters.timeouts++;
    return 0;
}

#endif /* HYDRA_UMC_GATEWAY_STACK_A_H */
