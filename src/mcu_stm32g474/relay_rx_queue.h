/* =============================================================================
 * HYDRA-UMC - src/mcu_stm32g474/relay_rx_queue.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0 - see LICENSE
 * =============================================================================
 * The Robot Controller Board's FDCAN2 (downlink) capture ring buffer, pulled
 * out of RobotControllerRelay.c into a HAL-free header so its behaviour - a
 * bounded ring that drops the OLDEST frame when full ("freshest data over a
 * growing backlog", the same policy VISION-STREAMER's own FrameBuffer uses) -
 * can be regression-tested on the host with plain gcc, no G474 board.
 *
 * Header-only + `static inline`: RobotControllerRelay.c gets exactly the same
 * machine code it had when this was written inline; a host test #includes the
 * same declarations directly. No STM32 HAL, no FreeRTOS - only <stdint.h> and
 * <string.h>.
 */
#ifndef HYDRA_UMC_RELAY_RX_QUEUE_H
#define HYDRA_UMC_RELAY_RX_QUEUE_H

#include <stdint.h>
#include <string.h>

#define RELAY_RX_QUEUE_DEPTH 8

typedef struct {
    uint16_t can_id;
    uint8_t dlc;
    uint8_t data[8];
} RelayCapturedFrame_t;

typedef struct {
    RelayCapturedFrame_t slots[RELAY_RX_QUEUE_DEPTH];
    uint8_t head;   /* next slot to fill  */
    uint8_t tail;   /* next slot to drain */
    uint8_t count;
} RelayRxQueue_t;

static inline void RelayRxQueue_Reset(RelayRxQueue_t *q)
{
    q->head = 0;
    q->tail = 0;
    q->count = 0;
}

static inline void RelayRxQueue_Push(RelayRxQueue_t *q, uint16_t can_id,
                                     const uint8_t *data, uint8_t dlc)
{
    if (q->count >= RELAY_RX_QUEUE_DEPTH) {
        /* Queue full - drop the OLDEST entry to make room rather than the
         * newest (which would silently discard whatever the operator is
         * actively waiting on right now). */
        q->tail = (uint8_t)((q->tail + 1) % RELAY_RX_QUEUE_DEPTH);
        q->count--;
    }
    RelayCapturedFrame_t *slot = &q->slots[q->head];
    slot->can_id = can_id;
    slot->dlc = dlc > 8 ? 8 : dlc;
    memcpy(slot->data, data, slot->dlc);
    q->head = (uint8_t)((q->head + 1) % RELAY_RX_QUEUE_DEPTH);
    q->count++;
}

static inline uint8_t RelayRxQueue_Pop(RelayRxQueue_t *q, RelayCapturedFrame_t *out)
{
    if (q->count == 0) return 0;
    *out = q->slots[q->tail];
    q->tail = (uint8_t)((q->tail + 1) % RELAY_RX_QUEUE_DEPTH);
    q->count--;
    return 1;
}

#endif /* HYDRA_UMC_RELAY_RX_QUEUE_H */
