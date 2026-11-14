/* =============================================================================
 * HYDRA-UMC Firmware - src/mcu_stm32h757/Common/gateway_self.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * gateway_self.h - the `self` callback of the gateway of the CM7 (gateway_dispatch.h): what the gateway core answers about itself on tier 0 (HAL-free, host-testable). It answers
 * the query of the error counters of the bus (frame type 0x0B) with the counters of FDCAN1 as the CM4 of the H745 board does: type 0x0C, two bytes, the transmit error counter and the
 * receive error counter (7 bits). Any other frame type is not answered (the gateway counts it as silent), and so is the query when the counters cannot be read: silence is the
 * truthful answer, not a counter of zero. The counters are reached only through a callback (CM7/Fdcan1Start.c on the board, a double in tests/test_gateway_self.c). The update
 * frames of tier 0 are not handled here. Nothing here has run on a board.
 * ============================================================================= */
#ifndef HYDRA_UMC_GATEWAY_SELF_H
#define HYDRA_UMC_GATEWAY_SELF_H

#include <stdint.h>
#include <string.h>

#include "gateway_dispatch.h"

#define GSELF_QUERY_ERROR_COUNTERS 0x0Bu
#define GSELF_ERROR_COUNTERS_RESPONSE 0x0Cu
#define GSELF_REC_MASK 0x7Fu

typedef struct {
    /* reads the counters of the bus; returns 1 when it could */
    int (*error_counters)(void *ctx, uint8_t *tec, uint8_t *rec);
    void *ctx;
    uint32_t answered, unavailable, ignored;
} GatewaySelf_t;

/* Returns 0 when the callback is missing. */
static inline int GatewaySelf_Init(GatewaySelf_t *s, int (*error_counters)(void *, uint8_t *, uint8_t *), void *ctx)
{
    if (!error_counters) return 0;
    memset(s, 0, sizeof *s);
    s->error_counters = error_counters;
    s->ctx = ctx;
    return 1;
}

/* The `self` callback of GatewayOps: `ctx` is the GatewaySelf_t. Returns 1 with the reply filled, 0 when there is nothing to say. */
static inline int GatewaySelf_Handle(void *ctx, uint8_t type, const uint8_t *payload, uint8_t dlc, GatewayReply *reply)
{
    GatewaySelf_t *s = (GatewaySelf_t *)ctx;
    (void)payload;
    (void)dlc;
    if (type != GSELF_QUERY_ERROR_COUNTERS) {
        s->ignored++;
        return 0;
    }
    uint8_t tec = 0u, rec = 0u;
    if (!s->error_counters(s->ctx, &tec, &rec)) {
        s->unavailable++;
        return 0;
    }
    memset(reply, 0, sizeof *reply);
    reply->type = GSELF_ERROR_COUNTERS_RESPONSE;
    reply->dlc = 2u;
    reply->payload[0] = tec;
    reply->payload[1] = (uint8_t)(rec & GSELF_REC_MASK);
    s->answered++;
    return 1;
}

#endif /* HYDRA_UMC_GATEWAY_SELF_H */
