/* =============================================================================
 * HYDRA-UMC Firmware - src/mcu_stm32h757/CM4/zones_frame.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * zones_frame.h - the filter of the brain for the frame of the safety-zones service on identifier 0x002 (HAL-free, host-testable). The twin of tools/zones_frame.py (read its header):
 * the brain has its OWN filter for the identifier, checks the frame and asks for a TRIP with the reason safety-zone (ESTOP_REASON_SAFETY_ZONE) when the frame is a stop request, is damaged
 * (a damaged frame can only cause a stop) or when the heartbeats that had started stop coming. A heartbeat refreshes the clock only when its sequence is newer. It never trips before the
 * first heartbeat and is armed again by the next one after a loss. The switch HYDRA_ZONES_CAN_FILTER ships off: with it off every function answers 0 and the brain does not even
 * program the filter. tests/test_zones_frame.c runs it against the vectors that tools/gen_zones_vectors.py writes from the Python reference. Nothing here has run on a board.
 * ============================================================================= */
#ifndef ZONES_FRAME_H
#define ZONES_FRAME_H

#include <stdint.h>

#ifndef HYDRA_ZONES_CAN_FILTER
#define HYDRA_ZONES_CAN_FILTER 0 /* the switch: off by default */
#endif
#ifndef ZONES_MAX_AGE_MS
#define ZONES_MAX_AGE_MS 1000u /* the oldest heartbeat that still counts; the service sends one every 100 ms */
#endif

#define ZONES_CAN_ID 0x002u
#define ZONES_KIND_STOP 0x01u
#define ZONES_KIND_HEARTBEAT 0x02u
#define ZONES_VERSION 1u

typedef struct {
    uint8_t enabled;
    uint8_t seen;
    uint8_t lost;
    uint16_t last_sequence;
    uint32_t last_heartbeat_ms;
    uint32_t trips;
    uint32_t ignored;
} ZonesFilter_t;

static inline uint8_t Zones_Crc8(const uint8_t *data, uint32_t length)
{
    uint8_t crc = 0;
    for (uint32_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (uint8_t)((crc & 0x80u) ? (((unsigned)crc << 1) ^ 0x07u) : ((unsigned)crc << 1));
        }
    }
    return crc;
}

static inline void ZonesFilter_Init(ZonesFilter_t *z)
{
    z->enabled = (uint8_t)(HYDRA_ZONES_CAN_FILTER ? 1u : 0u);
    z->seen = 0;
    z->lost = 0;
    z->last_sequence = 0;
    z->last_heartbeat_ms = 0;
    z->trips = 0;
    z->ignored = 0;
}

/* 1 when the brain must trip (reason safety-zone) because of this frame. */
static inline uint8_t ZonesFilter_OnFrame(ZonesFilter_t *z, uint32_t now_ms, uint32_t can_id, const uint8_t *data, uint8_t dlc)
{
    uint16_t sequence;
    if (!z->enabled || can_id != ZONES_CAN_ID) {
        return 0;
    }
    if (dlc != 8u || data[1] != ZONES_VERSION || data[7] != Zones_Crc8(data, 7u)) {
        z->trips++;
        return 1; /* a damaged frame can only cause a stop */
    }
    sequence = (uint16_t)(data[2] | ((uint16_t)data[3] << 8));
    if (data[0] == ZONES_KIND_STOP) {
        z->trips++;
        return 1;
    }
    if (data[0] == ZONES_KIND_HEARTBEAT) {
        uint16_t ahead = (uint16_t)(sequence - z->last_sequence);
        if (z->seen && !(ahead > 0u && ahead < 0x8000u)) {
            z->ignored++; /* a replayed or old heartbeat does not refresh the clock */
            return 0;
        }
        z->seen = 1;
        z->lost = 0;
        z->last_sequence = sequence;
        z->last_heartbeat_ms = now_ms;
        return 0;
    }
    z->ignored++; /* a valid frame of a kind this version does not know */
    return 0;
}

/* 1, once, when the heartbeats that had started stop coming for longer than ZONES_MAX_AGE_MS. */
static inline uint8_t ZonesFilter_Tick(ZonesFilter_t *z, uint32_t now_ms)
{
    if (!z->enabled || !z->seen || z->lost) {
        return 0;
    }
    if ((uint32_t)(now_ms - z->last_heartbeat_ms) > ZONES_MAX_AGE_MS) {
        z->lost = 1;
        z->trips++;
        return 1;
    }
    return 0;
}

#endif /* ZONES_FRAME_H */
