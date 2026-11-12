/* =============================================================================
 * HYDRA-UMC Firmware - src/ui_h757/core/ui_link.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * The host (master) side of the exchange with the Jetson for the interface of the display, in C: the twin of the master side of tools/ui_link.py (src/ui_h757/spec/jetson_link.json
 * defines the frames). It polls the status, which is the heartbeat of the view-model, fetches the still picture of a camera chunk by chunk and reports what it heard as events the
 * caller feeds to ui_core. No HAL and no malloc: the SPI transaction is a function the caller gives and the picture buffer is the caller's. The time comes in with every call.
 * Nothing here has run on a board or against a Jetson. Evidence: simulator (tests/test_ui_link_c.py replays transcripts recorded from the Python reference).
 */
#ifndef UI_LINK_H
#define UI_LINK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UI_LINK_SYNC 0x53u
#define UI_LINK_MAX_PAYLOAD 260u
#define UI_LINK_FRAME_OVERHEAD 10u
#define UI_LINK_MAX_FRAME (UI_LINK_FRAME_OVERHEAD + UI_LINK_MAX_PAYLOAD)
#define UI_LINK_CHUNK_BYTES 240u
#define UI_LINK_T_STATUS_REQ 0x10u
#define UI_LINK_T_SNAPSHOT_REQ 0x11u
#define UI_LINK_T_STATUS 0x90u
#define UI_LINK_T_SNAPSHOT 0x91u
#define UI_LINK_T_SNAPSHOT_NONE 0x9Fu

typedef enum { UI_LINK_EV_HEARTBEAT = 0, UI_LINK_EV_SPI_OK, UI_LINK_EV_SPI_ERROR } ui_link_event_kind_t;

typedef struct {
    uint8_t kind; /* ui_link_event_kind_t */
    uint8_t zone_level; /* of a heartbeat: 0 ready .. 5 unknown */
} ui_link_event_t;

/* One SPI transaction: send the request, return the length of the answer written to `answer`, or -1 when nothing (valid or not) came back in time. */
typedef int32_t (*ui_link_transact_t)(void *context, const uint8_t *request, uint32_t request_length, uint8_t *answer, uint32_t capacity, uint64_t now_ms);

typedef struct {
    ui_link_transact_t transact;
    void *context;
    uint8_t *picture;            /* the caller's buffer, picture_capacity bytes */
    uint32_t picture_capacity;   /* the largest JPEG accepted (jpeg_max_factor times the usual size) */
    uint32_t status_period_ms;
    uint32_t snapshot_period_ms;
    /* state */
    uint16_t sequence;
    int64_t last_status;
    int64_t last_snapshot;
    uint16_t last_picture_id;
    int32_t assembling_id;       /* -1 when no picture is being built */
    uint32_t assembling_total;
    uint32_t assembling_length;
    uint8_t status_seen;
    uint8_t status_picture_available;
    /* results */
    uint32_t pictures_done;
    uint32_t picture_length;     /* of the last complete picture; its bytes stay in `picture` until the next chunk arrives */
    uint32_t dropped;
    uint32_t transactions;
} ui_link_master_t;

void ui_link_init(ui_link_master_t *m, ui_link_transact_t transact, void *context, uint8_t *picture, uint32_t picture_capacity, uint32_t status_period_ms, uint32_t snapshot_period_ms);

/* Runs what is due at this time and writes the events for the view-model, in order, into `events` (room for at least 2). Returns how many. */
uint32_t ui_link_step(ui_link_master_t *m, uint64_t now_ms, ui_link_event_t *events, uint32_t capacity);

/* Frame helpers, public for the tests: build a frame (returns its length) and check one (returns 1 and the parts when it is whole and its CRC is right). */
uint32_t ui_link_encode(uint8_t kind, uint16_t sequence, const uint8_t *payload, uint32_t payload_length, uint8_t *frame);
int ui_link_decode(const uint8_t *frame, uint32_t length, uint8_t *kind, uint16_t *sequence, const uint8_t **payload, uint32_t *payload_length);

#ifdef __cplusplus
}
#endif

#endif /* UI_LINK_H */
