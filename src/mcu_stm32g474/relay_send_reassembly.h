/* =============================================================================
 * HYDRA-UMC - src/mcu_stm32g474/relay_send_reassembly.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Puts the RELAY_SEND fragments back together into the one CAN frame they
 * came from. The Kinematic Brain (and the CM5 tunnel) cut a frame of up to 8
 * data bytes into at most two fragments - 5 bytes, then the rest - each
 * carrying [target id hi, target id lo, total dlc, data...]. The tool head
 * bootloader only accepts whole 8-byte (or 4-byte) frames, so they must go
 * out on the tool-head bus as ONE frame, not as two short ones.
 *
 * A fragment of exactly 5 bytes in a frame longer than 5 is the first one;
 * the second is always 1-3 bytes, so the two cannot be mixed up. A frame of
 * 5 bytes or less is a single fragment.
 *
 * HAL-free so it can be tested on the host with plain gcc.
 */
#ifndef HYDRA_UMC_RELAY_SEND_REASSEMBLY_H
#define HYDRA_UMC_RELAY_SEND_REASSEMBLY_H

#include <stdint.h>
#include <string.h>

#define RELAY_SEND_FIRST_FRAGMENT_BYTES   5U
#define RELAY_SEND_REASSEMBLY_TIMEOUT_MS  20U /* the second fragment follows the first at once; older than this and it is gone */
#define RELAY_SEND_MAX_CAN_ID             0x7FFU

typedef struct {
    uint8_t  pending;
    uint16_t target_id;
    uint8_t  total_dlc;
    uint8_t  have;
    uint8_t  data[8];
    uint32_t start_ms;
} RelaySendReassembly_t;

static inline void RelaySendReassembly_Reset(RelaySendReassembly_t *r)
{
    memset(r, 0, sizeof(*r));
}

/* Feeds one fragment. Returns 1 and fills out_id/out_data/out_dlc when a whole
 * frame is ready to send; returns 0 when more is awaited or the fragment was
 * dropped (target id above 0x7FF, total above 8, lengths that do not fit, a
 * second fragment with no first one, or one that arrived too late). */
static inline uint8_t RelaySendReassembly_Feed(
    RelaySendReassembly_t *r, uint32_t now_ms,
    uint16_t target_id, uint8_t total_dlc, const uint8_t *fragment, uint8_t fragment_len,
    uint16_t *out_id, uint8_t out_data[8], uint8_t *out_dlc)
{
    if (target_id > RELAY_SEND_MAX_CAN_ID || total_dlc > 8U || fragment_len > 8U) {
        return 0;
    }
    if (r->pending && (uint32_t)(now_ms - r->start_ms) >= RELAY_SEND_REASSEMBLY_TIMEOUT_MS) {
        r->pending = 0;
    }

    uint8_t is_first = (total_dlc <= RELAY_SEND_FIRST_FRAGMENT_BYTES) || (fragment_len == RELAY_SEND_FIRST_FRAGMENT_BYTES);
    if (is_first) {
        uint8_t expected = total_dlc < RELAY_SEND_FIRST_FRAGMENT_BYTES ? total_dlc : (uint8_t)RELAY_SEND_FIRST_FRAGMENT_BYTES;
        r->pending = 0;
        if (fragment_len != expected) return 0;
        memset(r->data, 0, sizeof(r->data));
        memcpy(r->data, fragment, fragment_len);
        r->have = fragment_len;
        if (r->have == total_dlc) {
            *out_id = target_id;
            memcpy(out_data, r->data, 8);
            *out_dlc = total_dlc;
            return 1;
        }
        r->pending = 1;
        r->target_id = target_id;
        r->total_dlc = total_dlc;
        r->start_ms = now_ms;
        return 0;
    }

    /* second fragment */
    if (!r->pending || r->target_id != target_id || r->total_dlc != total_dlc) return 0;
    r->pending = 0;
    if ((uint8_t)(r->have + fragment_len) != total_dlc) return 0;
    memcpy(&r->data[r->have], fragment, fragment_len);
    *out_id = target_id;
    memcpy(out_data, r->data, 8);
    *out_dlc = total_dlc;
    return 1;
}

#endif /* HYDRA_UMC_RELAY_SEND_REASSEMBLY_H */
