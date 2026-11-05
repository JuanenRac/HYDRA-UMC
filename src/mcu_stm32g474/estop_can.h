/*
 * =============================================================================
 * estop_can.h - the emergency stop frame on identifier 0x001 and the state machine of a node (HAL-free, host-testable)
 * PROJECT: HYDRA-UMC
 * AUTHOR: JuanenRac (Electro Hobby 3D) - electrohobby3d@gmail.com
 * LICENSE: GPL-3.0-or-later - see repo root LICENSE
 *
 * C twin of tools/estop_can.py. docs/ESTOP_CAN.md is the contract, tests/estop_vectors.json holds the frames and the scenarios, and
 * tests/test_estop_can.c replays all of them against this file. The same text is kept in src/mcu_stm32g474/ (controller board) and in
 * src/mcu_stm32h745/CM4/ (kinematic brain), because each target builds from its own folder; tests/test_node_headers_parity.py keeps the two
 * copies identical.
 *
 * The rule that shapes everything: a stop must never be refused. Any frame on 0x001 that is not a well-formed RELEASE or ALIVE is treated as
 * a TRIP. Only the way out of a stop is checked. This is not a certified safety function and never replaces a hardware emergency stop.
 * =============================================================================
 */
#ifndef ESTOP_CAN_H
#define ESTOP_CAN_H

#include <stdint.h>
#include <string.h>

#define ESTOP_CAN_ID 0x001u

#define ESTOP_KIND_TRIP    0x01u
#define ESTOP_KIND_RELEASE 0x02u
#define ESTOP_KIND_ALIVE   0x03u

#define ESTOP_ALIVE_OK      0x00u
#define ESTOP_ALIVE_TRIPPED 0x01u

#define ESTOP_SOURCE_BRAIN      0x00u
#define ESTOP_SOURCE_SLOT_FIRST 0x01u
#define ESTOP_SOURCE_SLOT_LAST  0x08u
#define ESTOP_SOURCE_CM5        0x10u

#define ESTOP_REASON_OPERATOR     0x01u
#define ESTOP_REASON_STOP_ALL     0x02u
#define ESTOP_REASON_HARDWARE     0x03u
#define ESTOP_REASON_SAFETY_ZONE  0x04u
#define ESTOP_REASON_HOST_LINK    0x05u
#define ESTOP_REASON_CONTROLLER   0x06u
#define ESTOP_REASON_BUS_FAULT    0x07u
#define ESTOP_REASON_POWER_LOSS   0x08u
#define ESTOP_REASON_UNSPECIFIED  0xFFu

/* Timing of docs/bus_contract.json (section emergency_stop, timing_ms). Proposals until the bench rows 17, 18, 20 and 21 confirm them. */
#ifndef ESTOP_ALIVE_TIMEOUT_MS
#define ESTOP_ALIVE_TIMEOUT_MS 100u
#endif

#define ESTOP_STATE_LATCHED 0u
#define ESTOP_STATE_READY   1u

typedef struct {
    uint8_t kind;
    uint8_t detail;
    uint8_t source;
    uint8_t epoch;
    uint16_t seq;
} EstopFrame_t;

/* The 8 data bytes: kind, detail, source, epoch, sequence (little endian), then the complement of kind and detail. */
static inline void Estop_Encode(uint8_t out[8], uint8_t kind, uint8_t detail, uint8_t source, uint8_t epoch, uint16_t seq)
{
    out[0] = kind;
    out[1] = detail;
    out[2] = source;
    out[3] = epoch;
    out[4] = (uint8_t)(seq & 0xFFu);
    out[5] = (uint8_t)(seq >> 8);
    out[6] = (uint8_t)(kind ^ 0xFFu);
    out[7] = (uint8_t)(detail ^ 0xFFu);
}

static inline uint8_t Estop_ReasonKnown(uint8_t reason)
{
    return (uint8_t)((reason >= ESTOP_REASON_OPERATOR && reason <= ESTOP_REASON_POWER_LOSS) || reason == ESTOP_REASON_UNSPECIFIED);
}

/* 1 and the frame when the 8 bytes are well formed, 0 when they are not (a receiver then treats the frame as a TRIP). */
static inline uint8_t Estop_Decode(const uint8_t *data, uint8_t dlc, EstopFrame_t *out)
{
    if (dlc != 8u) return 0;
    const uint8_t kind = data[0], detail = data[1], source = data[2];
    if (data[6] != (uint8_t)(kind ^ 0xFFu) || data[7] != (uint8_t)(detail ^ 0xFFu)) return 0;
    if (kind == ESTOP_KIND_TRIP) {
        const uint8_t source_ok = (uint8_t)(source == ESTOP_SOURCE_BRAIN || source == ESTOP_SOURCE_CM5 ||
                                            (source >= ESTOP_SOURCE_SLOT_FIRST && source <= ESTOP_SOURCE_SLOT_LAST));
        if (!Estop_ReasonKnown(detail) || !source_ok) return 0;
    } else if (kind == ESTOP_KIND_RELEASE) {
        if (detail != 0x00u || source != ESTOP_SOURCE_BRAIN) return 0;
    } else if (kind == ESTOP_KIND_ALIVE) {
        if ((detail != ESTOP_ALIVE_OK && detail != ESTOP_ALIVE_TRIPPED) || source != ESTOP_SOURCE_BRAIN) return 0;
    } else {
        return 0;
    }
    out->kind = kind;
    out->detail = detail;
    out->source = source;
    out->epoch = data[3];
    out->seq = (uint16_t)(data[4] | ((uint16_t)data[5] << 8));
    return 1;
}

/* 1 when candidate is ahead of last in the modular order of 16 bits (1..32767 steps ahead); always 1 when there is no last yet. */
static inline uint8_t Estop_SeqNewer(uint16_t candidate, uint8_t have_last, uint16_t last)
{
    if (!have_last) return 1;
    const uint16_t step = (uint16_t)(candidate - last);
    return (uint8_t)(step >= 1u && step <= 0x7FFFu);
}

/* What every node that moves or heats something does with the frames on 0x001. Time is in milliseconds and may wrap (32 bits). */
typedef struct {
    uint8_t latched;          /* 1 = LATCHED (drivers off), 0 = READY */
    uint8_t drivers_enabled;  /* set only by EstopNode_Enable() from READY */
    uint8_t have_epoch;
    uint8_t epoch;
    uint8_t have_seq;
    uint16_t last_seq;
    uint8_t have_alive;
    uint32_t last_alive_ms;
} EstopNode_t;

static inline void EstopNode_Reset(EstopNode_t *n)
{
    memset(n, 0, sizeof *n);
    n->latched = 1u; /* power-up is LATCHED */
}

static inline void EstopNode_Latch_(EstopNode_t *n)
{
    n->latched = 1u;
    n->drivers_enabled = 0u;
}

static inline uint8_t EstopNode_Alive_(const EstopNode_t *n, uint32_t now_ms)
{
    return (uint8_t)(n->have_alive && (uint32_t)(now_ms - n->last_alive_ms) <= ESTOP_ALIVE_TIMEOUT_MS);
}

/* Feeds one frame received on 0x001 (any length): a malformed one is a TRIP. */
static inline void EstopNode_OnFrame(EstopNode_t *n, uint32_t now_ms, const uint8_t *data, uint8_t dlc)
{
    EstopFrame_t f;
    if (!Estop_Decode(data, dlc, &f)) {
        EstopNode_Latch_(n);
        return;
    }
    if (f.kind == ESTOP_KIND_TRIP) {
        EstopNode_Latch_(n);
        n->have_epoch = 1u;
        n->epoch = f.epoch;
        return;
    }
    const uint8_t newer = Estop_SeqNewer(f.seq, n->have_seq, n->last_seq);
    if (f.kind == ESTOP_KIND_ALIVE) {
        if (f.detail == ESTOP_ALIVE_TRIPPED) { /* recovers a TRIP this node never heard */
            EstopNode_Latch_(n);
            n->have_epoch = 1u;
            n->epoch = f.epoch;
            if (newer) {
                n->have_seq = 1u;
                n->last_seq = f.seq;
                n->have_alive = 1u;
                n->last_alive_ms = now_ms;
            }
            return;
        }
        if (!newer) return; /* a frozen or replayed sender does not keep the node alive */
        n->have_seq = 1u;
        n->last_seq = f.seq;
        n->have_alive = 1u;
        n->last_alive_ms = now_ms;
        return;
    }
    /* RELEASE: only from a latched node that heard a live master, for the epoch it knows, with a newer sequence. It never enables anything. */
    if (n->latched && newer && n->have_epoch && f.epoch == n->epoch && EstopNode_Alive_(n, now_ms)) {
        n->have_seq = 1u;
        n->last_seq = f.seq;
        n->latched = 0u;
    }
}

static inline void EstopNode_OnBusOff(EstopNode_t *n)
{
    EstopNode_Latch_(n); /* the bus coming back never releases anything */
}

/* Call every pass: a READY node that has not heard the master within the timeout becomes LATCHED. */
static inline void EstopNode_Tick(EstopNode_t *n, uint32_t now_ms)
{
    if (!n->latched && !EstopNode_Alive_(n, now_ms)) EstopNode_Latch_(n);
}

/* The normal enable command: refused unless the node is READY. 1 when accepted. */
static inline uint8_t EstopNode_Enable(EstopNode_t *n)
{
    if (n->latched) return 0;
    n->drivers_enabled = 1u;
    return 1;
}

/* ESTOP_STATE (slot_base + 0x1D, 4 bytes): state 0 LATCHED / 1 READY, epoch, last accepted sequence low and high. */
static inline void EstopNode_BuildState(const EstopNode_t *n, uint8_t out[4])
{
    out[0] = n->latched ? (uint8_t)ESTOP_STATE_LATCHED : (uint8_t)ESTOP_STATE_READY;
    out[1] = n->epoch;
    out[2] = (uint8_t)(n->last_seq & 0xFFu);
    out[3] = (uint8_t)(n->last_seq >> 8);
}

#endif /* ESTOP_CAN_H */
