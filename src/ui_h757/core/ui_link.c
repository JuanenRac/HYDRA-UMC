/* =============================================================================
 * HYDRA-UMC Firmware - src/ui_h757/core/ui_link.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * The twin of the master side of tools/ui_link.py (LinkMaster, Assembler, decode_status). Keep the two in the same order of steps: tests/test_ui_link_c.py replays transcripts
 * recorded from the Python reference and compares every request byte and every event.
 */
#include "ui_link.h"
#include "ui_pin.h" /* ui_crc32 */

#define STATUS_PAYLOAD 10u /* version, zone level, cpu, temperature (2), flags, uptime (4) */
#define SNAPSHOT_HEADER 10u /* picture id (2), total (4), offset (4) */
#define STATUS_VERSION 1u
#define FLAG_PICTURE 0x01u

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v);
    put16(p + 2, v >> 16);
}

static uint32_t get16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t get32(const uint8_t *p)
{
    return get16(p) | (get16(p + 2) << 16);
}

uint32_t ui_link_encode(uint8_t kind, uint16_t sequence, const uint8_t *payload, uint32_t payload_length, uint8_t *frame)
{
    uint32_t used = 6u + payload_length;
    frame[0] = UI_LINK_SYNC;
    frame[1] = kind;
    put16(frame + 2, sequence);
    put16(frame + 4, payload_length);
    for (uint32_t i = 0; i < payload_length; i++) {
        frame[6 + i] = payload[i];
    }
    put32(frame + used, ui_crc32(frame, used));
    return used + 4u;
}

int ui_link_decode(const uint8_t *frame, uint32_t length, uint8_t *kind, uint16_t *sequence, const uint8_t **payload, uint32_t *payload_length)
{
    uint32_t declared;
    if (length < UI_LINK_FRAME_OVERHEAD || get32(frame + length - 4u) != ui_crc32(frame, length - 4u)) {
        return 0;
    }
    declared = get16(frame + 4);
    if (frame[0] != UI_LINK_SYNC || declared > UI_LINK_MAX_PAYLOAD || length != UI_LINK_FRAME_OVERHEAD + declared) {
        return 0;
    }
    if (frame[1] != UI_LINK_T_STATUS && frame[1] != UI_LINK_T_SNAPSHOT && frame[1] != UI_LINK_T_SNAPSHOT_NONE) {
        return 0; /* only the types the Jetson may answer with */
    }
    *kind = frame[1];
    *sequence = (uint16_t)get16(frame + 2);
    *payload = frame + 6;
    *payload_length = declared;
    return 1;
}

void ui_link_init(ui_link_master_t *m, ui_link_transact_t transact, void *context, uint8_t *picture, uint32_t picture_capacity, uint32_t status_period_ms, uint32_t snapshot_period_ms)
{
    uint8_t *raw = (uint8_t *)m;
    for (uint32_t i = 0; i < sizeof(*m); i++) {
        raw[i] = 0;
    }
    m->transact = transact;
    m->context = context;
    m->picture = picture;
    m->picture_capacity = picture_capacity;
    m->status_period_ms = status_period_ms;
    m->snapshot_period_ms = snapshot_period_ms;
    m->last_status = -1000000000;
    m->last_snapshot = -1000000000;
    m->assembling_id = -1;
}

static void reset_assembly(ui_link_master_t *m)
{
    m->assembling_id = -1;
    m->assembling_total = 0;
    m->assembling_length = 0;
}

/* One request and its answer. Returns the length of the answer in the buffer, or -1. The sequence moves on every request. */
static int32_t ask(ui_link_master_t *m, uint8_t kind, const uint8_t *payload, uint32_t payload_length, uint8_t *answer, uint64_t now_ms)
{
    uint8_t request[UI_LINK_MAX_FRAME];
    uint32_t length;
    m->sequence = (uint16_t)(m->sequence + 1u);
    length = ui_link_encode(kind, m->sequence, payload, payload_length, request);
    m->transactions++;
    return m->transact(m->context, request, length, answer, UI_LINK_MAX_FRAME, now_ms);
}

/* The status heartbeat: 1 when the answer is a whole, right, fresh status; then the zone level is in *zone. */
static int read_status(ui_link_master_t *m, const uint8_t *answer, int32_t length, uint8_t *zone)
{
    uint8_t kind;
    uint16_t sequence;
    const uint8_t *payload;
    uint32_t payload_length;
    if (length < 0 || !ui_link_decode(answer, (uint32_t)length, &kind, &sequence, &payload, &payload_length) || sequence != m->sequence) {
        return 0;
    }
    if (kind != UI_LINK_T_STATUS || payload_length != STATUS_PAYLOAD || payload[0] != STATUS_VERSION || payload[1] > 5u || payload[2] > 100u) {
        return 0;
    }
    *zone = payload[1];
    m->status_seen = 1;
    m->status_picture_available = (payload[5] & FLAG_PICTURE) != 0u;
    return 1;
}

/* Feeds one chunk to the picture being built; returns 1 when it completes a good picture. Anything odd drops the picture and nothing of it is ever shown. */
static int feed(ui_link_master_t *m, uint32_t picture_id, uint32_t total, uint32_t offset, const uint8_t *chunk, uint32_t chunk_length)
{
    const uint8_t *p = m->picture;
    if (total == 0u || total > m->picture_capacity) {
        m->dropped++;
        reset_assembly(m);
        return 0;
    }
    if (m->assembling_id != (int32_t)picture_id) {
        if (offset != 0u) {
            m->dropped++;
            return 0;
        }
        m->assembling_id = (int32_t)picture_id;
        m->assembling_total = total;
        m->assembling_length = 0;
    }
    if (total != m->assembling_total || offset != m->assembling_length || offset + chunk_length > total) {
        m->dropped++;
        reset_assembly(m);
        return 0;
    }
    for (uint32_t i = 0; i < chunk_length; i++) {
        m->picture[offset + i] = chunk[i];
    }
    m->assembling_length += chunk_length;
    if (m->assembling_length < total) {
        return 0;
    }
    reset_assembly(m);
    if (total < 2u || p[0] != 0xFFu || p[1] != 0xD8u || p[total - 2u] != 0xFFu || p[total - 1u] != 0xD9u) {
        m->dropped++;
        return 0;
    }
    m->picture_length = total;
    return 1;
}

/* Fetches chunks until a picture is complete or something goes wrong; a failure costs only the picture. */
static void snapshot(ui_link_master_t *m, uint64_t now_ms)
{
    uint8_t answer[UI_LINK_MAX_FRAME];
    uint8_t request[7];
    m->last_snapshot = (int64_t)now_ms;
    for (uint32_t turn = 0; turn < m->picture_capacity / UI_LINK_CHUNK_BYTES + 2u; turn++) {
        int32_t length;
        uint8_t kind;
        uint16_t sequence;
        const uint8_t *payload;
        uint32_t payload_length;
        request[0] = 0; /* camera 0 */
        put16(request + 1, m->last_picture_id);
        put32(request + 3, m->assembling_length);
        length = ask(m, UI_LINK_T_SNAPSHOT_REQ, request, sizeof(request), answer, now_ms);
        if (length < 0) {
            reset_assembly(m);
            return;
        }
        if (!ui_link_decode(answer, (uint32_t)length, &kind, &sequence, &payload, &payload_length) || sequence != m->sequence) {
            m->dropped++;
            reset_assembly(m);
            return;
        }
        if (kind == UI_LINK_T_SNAPSHOT_NONE) {
            return;
        }
        if (kind != UI_LINK_T_SNAPSHOT || payload_length < SNAPSHOT_HEADER) {
            reset_assembly(m);
            return;
        }
        {
            uint32_t picture_id = get16(payload);
            int done = feed(m, picture_id, get32(payload + 2), get32(payload + 6), payload + SNAPSHOT_HEADER, payload_length - SNAPSHOT_HEADER);
            if (m->assembling_id < 0 && !done) {
                return; /* dropped */
            }
            if (done) {
                m->pictures_done++;
                m->last_picture_id = (uint16_t)picture_id;
                return;
            }
        }
    }
}

uint32_t ui_link_step(ui_link_master_t *m, uint64_t now_ms, ui_link_event_t *events, uint32_t capacity)
{
    uint32_t count = 0;
    if ((int64_t)now_ms - m->last_status >= (int64_t)m->status_period_ms) {
        uint8_t answer[UI_LINK_MAX_FRAME];
        uint8_t zone = 0;
        int32_t length;
        m->last_status = (int64_t)now_ms;
        length = ask(m, UI_LINK_T_STATUS_REQ, 0, 0, answer, now_ms);
        if (read_status(m, answer, length, &zone)) {
            if (count + 2u <= capacity) {
                events[count].kind = UI_LINK_EV_HEARTBEAT;
                events[count].zone_level = zone;
                events[count + 1u].kind = UI_LINK_EV_SPI_OK;
                events[count + 1u].zone_level = 0;
                count += 2u;
            }
        } else if (count < capacity) {
            events[count].kind = UI_LINK_EV_SPI_ERROR;
            events[count].zone_level = 0;
            count++;
        }
    }
    if (m->status_seen && m->status_picture_available && (int64_t)now_ms - m->last_snapshot >= (int64_t)m->snapshot_period_ms) {
        snapshot(m, now_ms);
    }
    return count;
}
