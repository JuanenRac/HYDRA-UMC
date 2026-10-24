/* =============================================================================
 * HYDRA-UMC - src/mcu_stm32g474/relay_ids.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Which identifier the Robot Controller Board answers on, and how a captured
 * frame is cut into the fragments of an answer, pulled out of
 * RobotControllerRelay.c into a HAL-free header so a host test can run it.
 *
 * On CAN an identifier must have one emitter. The kinematic brain asks on
 * +0x10 (axis status) and +0x13 (relay receive); this board answers on +0x16
 * and +0x18, never on the identifier of the request (tools/can_ids.py).
 */
#ifndef HYDRA_UMC_RELAY_IDS_H
#define HYDRA_UMC_RELAY_IDS_H

#include <stdint.h>
#include <string.h>

#include "relay_rx_queue.h"

#define RELAY_REQUEST_AXIS_STATUS 0x10u
#define RELAY_REQUEST_RECV        0x13u
#define RELAY_ANSWER_AXIS_STATUS  0x16u
#define RELAY_ANSWER_RECV         0x18u

#define RELAY_FRAGMENT_HEADER_BYTES 3
#define RELAY_FRAGMENT_DATA_BYTES   5

/* The offset this board answers on for a request offset, or 0 when the offset is not a request that has an answer. */
static inline uint8_t RelayAnswerOffset(uint32_t request_offset)
{
    if (request_offset == RELAY_REQUEST_AXIS_STATUS) return (uint8_t)RELAY_ANSWER_AXIS_STATUS;
    if (request_offset == RELAY_REQUEST_RECV) return (uint8_t)RELAY_ANSWER_RECV;
    return 0;
}

/* The two error counters of the CAN controller as the bootloader reports them (OFS_ERROR_COUNTERS_RESPONSE, DLC 2): the transmit counter in
 * bits 7:0 and the receive counter in bits 14:8 of the FDCAN error counter register (RM0440, ECR). The application answers the same query
 * the bootloader does, so the CM5 can read the health of the bus whichever of the two is running. */
static inline void RelayErrorCounters(uint32_t ecr, uint8_t out[2])
{
    out[0] = (uint8_t)(ecr & 0xFFu);
    out[1] = (uint8_t)((ecr >> 8) & 0x7Fu);
}

typedef struct {
    uint8_t count;       /* 1 or 2 */
    uint8_t dlc[2];      /* data length of each fragment */
    uint8_t data[2][8];  /* the fragments */
} RelayAnswerFragments_t;

/* Cuts a captured frame into the one or two fragments of the answer to a relay receive request: 2-byte CAN identifier (big endian), the total
 * data length of the frame and up to 5 data bytes per fragment. The same layout KinematicBrainCan.c and relay_tunnel.py read. */
static inline void RelayBuildAnswerFragments(const RelayCapturedFrame_t *frame, RelayAnswerFragments_t *out)
{
    memset(out, 0, sizeof(*out));
    uint8_t total = frame->dlc > 8 ? 8 : frame->dlc;
    uint8_t first_len = total <= RELAY_FRAGMENT_DATA_BYTES ? total : RELAY_FRAGMENT_DATA_BYTES;
    out->data[0][0] = (uint8_t)(frame->can_id >> 8);
    out->data[0][1] = (uint8_t)(frame->can_id & 0xFF);
    out->data[0][2] = total;
    memcpy(&out->data[0][RELAY_FRAGMENT_HEADER_BYTES], frame->data, first_len);
    out->dlc[0] = (uint8_t)(RELAY_FRAGMENT_HEADER_BYTES + first_len);
    out->count = 1;
    if (total > RELAY_FRAGMENT_DATA_BYTES) {
        uint8_t second_len = (uint8_t)(total - RELAY_FRAGMENT_DATA_BYTES);
        out->data[1][0] = out->data[0][0];
        out->data[1][1] = out->data[0][1];
        out->data[1][2] = total;
        memcpy(&out->data[1][RELAY_FRAGMENT_HEADER_BYTES], frame->data + RELAY_FRAGMENT_DATA_BYTES, second_len);
        out->dlc[1] = (uint8_t)(RELAY_FRAGMENT_HEADER_BYTES + second_len);
        out->count = 2;
    }
}

#endif /* HYDRA_UMC_RELAY_IDS_H */
