/* =============================================================================
 * HYDRA-UMC Firmware - src/ui_h757/test/ui_link_test.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Host test of the master side of the Jetson link in C (src/ui_h757/core/ui_link.c). Every scenario of src/ui_h757/test/ui_link_vectors.h was recorded from the Python reference
 * (tools/ui_link.py, a Jetson double that answers, stays silent, corrupts, hangs in a picture, damages a chunk or changes the picture in the middle of a transfer): the C master
 * has to send the same request bytes at the same moments, report the same events and end with the same pictures. Evidence: simulator.
 *
 *   cc -std=c11 -Wall -Wextra -Isrc/ui_h757/core -Isrc/ui_h757/test -o ui_link_test src/ui_h757/test/ui_link_test.c src/ui_h757/core/ui_link.c src/ui_h757/core/ui_pin.c
 */
#include <stdio.h>
#include <string.h>

#include "ui_link.h"
#include "ui_pin.h"
#include "ui_link_vectors.h"

typedef struct {
    const ui_vec_link_scenario_t *scenario;
    uint32_t next;
    uint32_t mismatches;
    uint32_t first_step_txn;
} replay_t;

static int32_t replay_transact(void *context, const uint8_t *request, uint32_t request_length, uint8_t *answer, uint32_t capacity, uint64_t now_ms)
{
    replay_t *r = (replay_t *)context;
    const ui_vec_txn_t *t = &r->scenario->txn[r->next];
    r->next++;
    if (t->now != now_ms || t->request_length != request_length || memcmp(ui_link_pool + t->request_offset, request, request_length) != 0) {
        r->mismatches++;
        return -1;
    }
    if (t->answer_length < 0) {
        return -1;
    }
    if ((uint32_t)t->answer_length > capacity) {
        r->mismatches++;
        return -1;
    }
    memcpy(answer, ui_link_pool + t->answer_offset, (size_t)t->answer_length);
    return t->answer_length;
}

static int scenarios(void)
{
    int failures = 0;
    for (unsigned n = 0; n < UI_VEC_LINK_SCENARIOS; n++) {
        const ui_vec_link_scenario_t *sc = &ui_vec_link_scenarios[n];
        static uint8_t picture[4096];
        ui_link_master_t master;
        replay_t replay;
        replay.scenario = sc;
        replay.next = 0;
        replay.mismatches = 0;
        ui_link_init(&master, replay_transact, &replay, picture, sc->capacity, 100, 500);
        for (unsigned i = 0; i < sc->steps; i++) {
            const ui_vec_step_t *s = &sc->step[i];
            ui_link_event_t events[2];
            uint32_t count;
            uint32_t before = replay.next;
            uint32_t done_before = master.pictures_done;
            count = ui_link_step(&master, s->now, events, 2);
            if (before != s->first_txn || replay.next - before != s->txn_count || replay.mismatches != 0) {
                printf("  FAIL: %s step %u (t=%llu): transactions %u..%u, expected %u..%u, mismatches %u\n", sc->name, i, (unsigned long long)s->now, (unsigned)before,
                       (unsigned)replay.next, (unsigned)s->first_txn, (unsigned)(s->first_txn + s->txn_count), (unsigned)replay.mismatches);
                failures++;
                break;
            }
            if (count != s->event_count) {
                printf("  FAIL: %s step %u: %u events, expected %u\n", sc->name, i, (unsigned)count, (unsigned)s->event_count);
                failures++;
                break;
            }
            for (uint32_t e = 0; e < count; e++) {
                if (events[e].kind != s->events[e].kind || events[e].zone_level != s->events[e].zone) {
                    printf("  FAIL: %s step %u: event %u differs\n", sc->name, i, (unsigned)e);
                    failures++;
                    return failures;
                }
            }
            if (master.pictures_done != s->pictures_done || master.dropped != s->dropped) {
                printf("  FAIL: %s step %u: %u pictures, %u dropped; expected %u, %u\n", sc->name, i, (unsigned)master.pictures_done, (unsigned)master.dropped,
                       (unsigned)s->pictures_done, (unsigned)s->dropped);
                failures++;
                break;
            }
            if (master.pictures_done > done_before && (master.picture_length != s->picture_length || ui_crc32(picture, master.picture_length) != s->picture_crc)) {
                printf("  FAIL: %s step %u: the picture differs\n", sc->name, i);
                failures++;
                break;
            }
        }
    }
    printf("  %u transcripts replayed in C\n", (unsigned)UI_VEC_LINK_SCENARIOS);
    return failures;
}

static int frames(void)
{
    int failures = 0;
    uint8_t frame[UI_LINK_MAX_FRAME];
    uint8_t payload[260];
    uint8_t kind;
    uint16_t sequence;
    const uint8_t *body;
    uint32_t body_length;
    uint32_t length;
    for (unsigned i = 0; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(i * 7u);
    }
    length = ui_link_encode(UI_LINK_T_SNAPSHOT, 513, payload, 260, frame);
    if (length != 270u || !ui_link_decode(frame, length, &kind, &sequence, &body, &body_length) || kind != UI_LINK_T_SNAPSHOT || sequence != 513u || body_length != 260u ||
        memcmp(body, payload, 260) != 0) {
        printf("  FAIL: a frame does not round-trip\n");
        failures++;
    }
    for (uint32_t i = 0; i < length; i++) {
        uint8_t copy[UI_LINK_MAX_FRAME];
        memcpy(copy, frame, length);
        copy[i] ^= 0x01u;
        if (ui_link_decode(copy, length, &kind, &sequence, &body, &body_length)) {
            printf("  FAIL: a damaged frame was believed (byte %u)\n", (unsigned)i);
            failures++;
            break;
        }
    }
    if (ui_link_decode(frame, length - 1u, &kind, &sequence, &body, &body_length) || ui_link_decode(frame, 3, &kind, &sequence, &body, &body_length)) {
        printf("  FAIL: a truncated frame was believed\n");
        failures++;
    }
    length = ui_link_encode(UI_LINK_T_STATUS_REQ, 1, 0, 0, frame); /* a request is not something the Jetson may answer with */
    if (ui_link_decode(frame, length, &kind, &sequence, &body, &body_length)) {
        printf("  FAIL: a master frame was taken for an answer\n");
        failures++;
    }
    return failures;
}

int main(void)
{
    int failures = 0;
    failures += scenarios();
    failures += frames();
    if (failures == 0) {
        printf("ui_link: PASS\n");
        return 0;
    }
    printf("ui_link: FAIL (%d)\n", failures);
    return 1;
}
