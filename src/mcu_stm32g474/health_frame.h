/* =============================================================================
 * HYDRA-UMC - src/mcu_stm32g474/health_frame.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * The health frame of a controller board (HAL-free, host-testable): one classic CAN frame of 8 bytes that says how the firmware of the board is
 * doing, on the identifier slot_base + 0x11 (HEALTH_FRAME in tools/can_ids.py, emitter: the controller board).
 *
 *   byte 0  marker      0x48 ('H')
 *   byte 1  sequence    +1 for every frame sent
 *   byte 2  flags       bit 0 emergency stop latched, bit 1 bus-off seen since the last frame, bit 2 watchdog reset at start,
 *                       bit 3 a set-up call failed at start, bit 4 application confirmed healthy, bit 5 drivers enabled,
 *                       bit 6 a set-up call failed again and the board is going to the safe state (sent once, while the uplink still works)
 *   byte 3  tx errors   transmit error counter of the uplink controller
 *   byte 4  rx errors   receive error counter of the uplink controller
 *   byte 5  stalls      passes of the relay task that did not move the watchdog counter (saturates at 255)
 *   byte 6  uptime      seconds since the start, in units of 16 s, saturating at 255 (about 68 minutes)
 *   byte 7  check       XOR of bytes 0 to 6, then 0xA5
 *
 * The firmware sends it once per second when HYDRA_HEALTH_FRAME is 1 (default 0: the application sends nothing new). Evidence of the tests:
 * simulator (tests/test_app_support.c against the reference of tools/health_frame.py); the real counters are bench row 19.
 * ========================================================================== */
#ifndef HYDRA_UMC_HEALTH_FRAME_H
#define HYDRA_UMC_HEALTH_FRAME_H

#include <stdint.h>

#ifndef HYDRA_HEALTH_FRAME
#define HYDRA_HEALTH_FRAME 0
#endif

#define HEALTH_OFFSET        0x11u
#define HEALTH_MARKER        0x48u
#define HEALTH_PERIOD_MS     1000u
#define HEALTH_FLAG_ESTOP    0x01u
#define HEALTH_FLAG_BUSOFF   0x02u
#define HEALTH_FLAG_WDG_RST  0x04u
#define HEALTH_FLAG_INIT_BAD 0x08u
#define HEALTH_FLAG_CONFIRMED 0x10u
#define HEALTH_FLAG_ENABLED  0x20u
#define HEALTH_FLAG_INIT_SAFE 0x40u

typedef struct {
    uint8_t flags;
    uint8_t tx_errors;
    uint8_t rx_errors;
    uint8_t stalls;
    uint32_t uptime_ms;
    uint8_t seq;
    uint32_t last_sent_ms;
    uint8_t sent_once;
} HealthFrame_t;

static inline void HealthFrame_Reset(HealthFrame_t *h)
{
    h->flags = 0u; h->tx_errors = 0u; h->rx_errors = 0u; h->stalls = 0u;
    h->uptime_ms = 0u; h->seq = 0u; h->last_sent_ms = 0u; h->sent_once = 0u;
}

static inline uint8_t HealthFrame_Check(const uint8_t *frame)
{
    uint8_t x = 0xA5u;
    for (int i = 0; i < 7; i++) x ^= frame[i];
    return x;
}

static inline void HealthFrame_Build(HealthFrame_t *h, uint8_t out[8])
{
    const uint32_t units = h->uptime_ms / 16000u;
    out[0] = HEALTH_MARKER;
    out[1] = h->seq++;
    out[2] = h->flags;
    out[3] = h->tx_errors;
    out[4] = h->rx_errors;
    out[5] = h->stalls;
    out[6] = (uint8_t)(units > 255u ? 255u : units);
    out[7] = HealthFrame_Check(out);
    h->flags = (uint8_t)(h->flags & ~HEALTH_FLAG_BUSOFF);   /* bus-off is reported once */
}

/* 1 and the frame in `out` when it is time to send one. Time may wrap (32 bits). */
static inline uint8_t HealthFrame_Due(HealthFrame_t *h, uint32_t now_ms, uint8_t out[8])
{
#if !HYDRA_HEALTH_FRAME
    (void)h; (void)now_ms; (void)out;
    return 0u;
#else
    h->uptime_ms = now_ms;
    if (h->sent_once && (uint32_t)(now_ms - h->last_sent_ms) < HEALTH_PERIOD_MS) return 0u;
    h->sent_once = 1u;
    h->last_sent_ms = now_ms;
    HealthFrame_Build(h, out);
    return 1u;
#endif
}

#endif /* HYDRA_UMC_HEALTH_FRAME_H */
