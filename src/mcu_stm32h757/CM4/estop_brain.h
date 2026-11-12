/*
 * =============================================================================
 * estop_brain.h - what the kinematic brain sends on identifier 0x001 (HAL-free, host-testable)
 * PROJECT: HYDRA-UMC
 * AUTHOR: JuanenRac (Electro Hobby 3D) - electrohobby3d@gmail.com
 * LICENSE: GPL-3.0-or-later - see repo root LICENSE
 *
 * The master side of docs/ESTOP_CAN.md: it repeats TRIP while the stack is stopped, announces itself with ALIVE, and ends a stop only when a
 * person asked for it (EstopBrain_RequestRelease) and every expected controller board answered READY on ESTOP_STATE. tests/test_estop_can.c
 * runs it against the node state machine of estop_can.h, with the vectors of tests/estop_vectors.json for the node.
 *
 * This file only builds the frames and decides when; the caller sends them on FDCAN1 and feeds it what it hears. Time is in milliseconds and
 * may wrap (32 bits).
 * =============================================================================
 */
#ifndef ESTOP_BRAIN_H
#define ESTOP_BRAIN_H

#include "estop_can.h"

#ifndef ESTOP_TRIP_REPEAT_MS
#define ESTOP_TRIP_REPEAT_MS 5u
#endif
#ifndef ESTOP_ALIVE_PERIOD_MS
#define ESTOP_ALIVE_PERIOD_MS 20u
#endif
#ifndef ESTOP_RELEASE_REPEAT_MS
#define ESTOP_RELEASE_REPEAT_MS 20u
#endif
#ifndef ESTOP_RELEASE_ACK_TIMEOUT_MS
#define ESTOP_RELEASE_ACK_TIMEOUT_MS 500u
#endif

#define ESTOP_RELEASE_NONE    0u
#define ESTOP_RELEASE_PENDING 1u
#define ESTOP_RELEASE_DONE    2u
#define ESTOP_RELEASE_FAILED  3u

#define ESTOP_DUE(now, at) ((int32_t)((uint32_t)(now) - (uint32_t)(at)) >= 0)

typedef struct {
    uint8_t tripped;        /* 1 until a release was acknowledged by every expected board */
    uint8_t reason;         /* reason of the last TRIP the brain sent */
    uint8_t epoch;          /* counts stops: +1 each time the stack goes from running to tripped */
    uint16_t seq;           /* +1 on every frame transmitted on 0x001 */
    uint8_t release_state;  /* ESTOP_RELEASE_* */
    uint32_t release_started_ms;
    uint8_t expected_mask;  /* bit n = the board of slot n has to answer READY */
    uint8_t acked_mask;
    uint8_t missing_mask;   /* after a failed release: the boards that did not answer */
    uint8_t trip_now;       /* a TRIP has to go out at once */
    uint32_t next_trip_ms;
    uint32_t next_alive_ms;
    uint32_t next_release_ms;
} EstopBrain_t;

/* Power-up: the brain starts LATCHED and announces it, as every node does. */
static inline void EstopBrain_Reset(EstopBrain_t *b, uint32_t now_ms)
{
    memset(b, 0, sizeof *b);
    b->tripped = 1u;
    b->reason = ESTOP_REASON_UNSPECIFIED;
    b->next_trip_ms = now_ms;
    b->next_alive_ms = now_ms;
    b->next_release_ms = now_ms;
}

/* A stop from the brain itself (operator, stop-all from the server, hardware input, host link lost, bus fault...). Idempotent while already stopped,
 * except that it cancels a release in progress: the TRIP latches the boards that already turned READY. */
static inline void EstopBrain_Trip(EstopBrain_t *b, uint32_t now_ms, uint8_t reason)
{
    if (!b->tripped) {
        b->tripped = 1u;
        b->epoch = (uint8_t)(b->epoch + 1u);
        b->reason = reason;
        b->trip_now = 1u;
        b->next_trip_ms = (uint32_t)(now_ms + ESTOP_TRIP_REPEAT_MS);
        b->next_alive_ms = (uint32_t)(now_ms + ESTOP_ALIVE_PERIOD_MS);
        return;
    }
    if (b->release_state == ESTOP_RELEASE_PENDING) {
        b->release_state = ESTOP_RELEASE_NONE;
        b->reason = reason;
        b->trip_now = 1u;
        b->next_trip_ms = (uint32_t)(now_ms + ESTOP_TRIP_REPEAT_MS);
    }
}

/* A frame heard on 0x001 from somebody else. A TRIP stops the stack; a malformed frame is a TRIP too; a well-formed RELEASE or ALIVE would come from
 * another brain and is ignored. */
static inline void EstopBrain_OnFrame(EstopBrain_t *b, uint32_t now_ms, const uint8_t *data, uint8_t dlc)
{
    EstopFrame_t f;
    if (!Estop_Decode(data, dlc, &f)) {
        EstopBrain_Trip(b, now_ms, ESTOP_REASON_UNSPECIFIED);
    } else if (f.kind == ESTOP_KIND_TRIP) {
        EstopBrain_Trip(b, now_ms, f.detail);
    }
}

/* The ESTOP_STATE answer of the board of slot n (4 data bytes, see EstopNode_BuildState). Counts only while a release is pending and only for
 * the epoch of this stop. */
static inline void EstopBrain_OnSlotState(EstopBrain_t *b, uint8_t slot, const uint8_t *data, uint8_t dlc)
{
    if (slot > 7u || dlc < 4u || b->release_state != ESTOP_RELEASE_PENDING) return;
    if (data[0] == ESTOP_STATE_READY && data[1] == b->epoch) b->acked_mask = (uint8_t)(b->acked_mask | (1u << slot));
}

/* A person asked, in an authenticated administrative action, to end the stop; expected_mask lists the boards that have to confirm.
 * 1 when the release started. */
static inline uint8_t EstopBrain_RequestRelease(EstopBrain_t *b, uint32_t now_ms, uint8_t expected_mask)
{
    if (!b->tripped || b->release_state == ESTOP_RELEASE_PENDING) return 0;
    b->release_state = ESTOP_RELEASE_PENDING;
    b->release_started_ms = now_ms;
    b->expected_mask = expected_mask;
    b->acked_mask = 0u;
    b->missing_mask = 0u;
    b->next_alive_ms = now_ms;   /* the boards need a live master first, then the RELEASE */
    b->next_release_ms = now_ms;
    return 1;
}

static inline void EstopBrain_Build_(EstopBrain_t *b, uint8_t out[8], uint8_t kind, uint8_t detail)
{
    b->seq = (uint16_t)(b->seq + 1u);
    Estop_Encode(out, kind, detail, ESTOP_SOURCE_BRAIN, b->epoch, b->seq);
}

/* Call every millisecond or so. Returns 1 and the 8 data bytes of one frame to send on 0x001, or 0 when there is nothing to send now (call again
 * until it returns 0, at most a few times per pass). */
static inline uint8_t EstopBrain_Tick(EstopBrain_t *b, uint32_t now_ms, uint8_t out[8])
{
    if (b->release_state == ESTOP_RELEASE_PENDING) {
        if ((uint8_t)(b->acked_mask & b->expected_mask) == b->expected_mask) {
            b->release_state = ESTOP_RELEASE_DONE;
            b->tripped = 0u;
            b->next_alive_ms = now_ms;
        } else if ((uint32_t)(now_ms - b->release_started_ms) >= ESTOP_RELEASE_ACK_TIMEOUT_MS) {
            b->release_state = ESTOP_RELEASE_FAILED;
            b->missing_mask = (uint8_t)(b->expected_mask & (uint8_t)~b->acked_mask);
            b->trip_now = 1u;
            b->next_trip_ms = (uint32_t)(now_ms + ESTOP_TRIP_REPEAT_MS);
        }
    }
    if (b->trip_now) {
        b->trip_now = 0u;
        EstopBrain_Build_(b, out, ESTOP_KIND_TRIP, b->reason);
        return 1;
    }
    if (b->tripped && b->release_state != ESTOP_RELEASE_PENDING) {
        if (ESTOP_DUE(now_ms, b->next_trip_ms)) {
            b->next_trip_ms = (uint32_t)(now_ms + ESTOP_TRIP_REPEAT_MS);
            EstopBrain_Build_(b, out, ESTOP_KIND_TRIP, b->reason);
            return 1;
        }
        if (ESTOP_DUE(now_ms, b->next_alive_ms)) {
            b->next_alive_ms = (uint32_t)(now_ms + ESTOP_ALIVE_PERIOD_MS);
            EstopBrain_Build_(b, out, ESTOP_KIND_ALIVE, ESTOP_ALIVE_TRIPPED);
            return 1;
        }
        return 0;
    }
    /* Running, or a release in progress: ALIVE says "not tripped" so that a board that already turned READY is not latched again by the next ALIVE. */
    if (ESTOP_DUE(now_ms, b->next_alive_ms)) {
        b->next_alive_ms = (uint32_t)(now_ms + ESTOP_ALIVE_PERIOD_MS);
        EstopBrain_Build_(b, out, ESTOP_KIND_ALIVE, ESTOP_ALIVE_OK);
        return 1;
    }
    if (b->release_state == ESTOP_RELEASE_PENDING && ESTOP_DUE(now_ms, b->next_release_ms)) {
        b->next_release_ms = (uint32_t)(now_ms + ESTOP_RELEASE_REPEAT_MS);
        EstopBrain_Build_(b, out, ESTOP_KIND_RELEASE, 0x00u);
        return 1;
    }
    return 0;
}

#endif /* ESTOP_BRAIN_H */
