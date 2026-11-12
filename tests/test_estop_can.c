/* =============================================================================
 * HYDRA-UMC Firmware - host test of estop_can.h (frame and node) and, for the kinematic brain, of estop_brain.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * The frames and the scenarios are the ones of tests/estop_vectors.json, read through tests/estop_vectors.h (tools/gen_node_vectors.py).
 * Built twice: against src/mcu_stm32g474 (the node alone) and against src/mcu_stm32h757/CM4 with ESTOP_TEST_BRAIN (the node and the brain).
 */
#include <stdio.h>
#include <string.h>
#include "estop_can.h"
#ifdef ESTOP_TEST_BRAIN
#include "estop_brain.h"
#endif
#include "estop_vectors.h"

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } } while (0)

static void test_frames(void)
{
    for (unsigned i = 0; i < sizeof estop_vector_frames / sizeof estop_vector_frames[0]; i++) {
        const EstopVectorFrame *v = &estop_vector_frames[i];
        uint8_t made[8];
        Estop_Encode(made, v->kind, v->detail, v->source, v->epoch, v->seq);
        CHECK(memcmp(made, v->data, 8) == 0, v->name);
        EstopFrame_t f;
        CHECK(Estop_Decode(v->data, 8, &f) == 1, v->name);
        CHECK(f.kind == v->kind && f.detail == v->detail && f.source == v->source && f.epoch == v->epoch && f.seq == v->seq, v->name);
    }
    for (unsigned i = 0; i < sizeof estop_vector_malformed / sizeof estop_vector_malformed[0]; i++) {
        const EstopVectorMalformed *v = &estop_vector_malformed[i];
        EstopFrame_t f;
        CHECK(Estop_Decode(v->data, v->dlc, &f) == 0, v->name);
    }
}

static void test_sequence_order(void)
{
    CHECK(Estop_SeqNewer(1, 0, 0) == 1, "no last yet");
    CHECK(Estop_SeqNewer(11, 1, 10) == 1, "next");
    CHECK(Estop_SeqNewer(0, 1, 65535) == 1, "wrap");
    CHECK(Estop_SeqNewer(10, 1, 10) == 0, "same");
    CHECK(Estop_SeqNewer(10, 1, 11) == 0, "older");
    CHECK(Estop_SeqNewer((uint16_t)(10u + 0x8000u), 1, 10) == 0, "half way round is not newer");
}

static void test_scenarios(void)
{
    CHECK(ESTOP_VECTOR_ALIVE_TIMEOUT_MS == ESTOP_ALIVE_TIMEOUT_MS, "timeout of the vectors is the timeout of the header");
    for (unsigned s = 0; s < sizeof estop_vector_scenarios / sizeof estop_vector_scenarios[0]; s++) {
        const EstopVectorScenario *sc = &estop_vector_scenarios[s];
        EstopNode_t node;
        EstopNode_Reset(&node);
        CHECK(node.latched == 1 && node.drivers_enabled == 0, sc->name);
        for (unsigned k = 0; k < sc->steps; k++) {
            const EstopVectorStep *st = &estop_vector_steps[sc->first_step + k];
            int accepted = -1;
            if (st->action == ESTOP_VECTOR_ACTION_FRAME) EstopNode_OnFrame(&node, st->t, st->data, st->dlc);
            else if (st->action == ESTOP_VECTOR_ACTION_TICK) EstopNode_Tick(&node, st->t);
            else if (st->action == ESTOP_VECTOR_ACTION_BUS_OFF) EstopNode_OnBusOff(&node);
            else if (st->action == ESTOP_VECTOR_ACTION_ENABLE) accepted = EstopNode_Enable(&node);
            if (st->expect_state >= 0) CHECK((int)(node.latched ? 0 : 1) == st->expect_state, sc->name);
            if (st->expect_drivers >= 0) CHECK((int)node.drivers_enabled == st->expect_drivers, sc->name);
            if (st->expect_accepted >= 0) CHECK(accepted == st->expect_accepted, sc->name);
        }
    }
}

static void test_a_trip_cuts_the_drivers_in_every_state(void)
{
    const uint8_t reasons[] = {1, 2, 3, 4, 5, 6, 7, 8, 0xFF};
    for (unsigned i = 0; i < sizeof reasons; i++) {
        EstopNode_t node;
        EstopNode_Reset(&node);
        uint8_t data[8];
        Estop_Encode(data, ESTOP_KIND_TRIP, 1, ESTOP_SOURCE_BRAIN, 3, 1);
        EstopNode_OnFrame(&node, 0, data, 8);
        Estop_Encode(data, ESTOP_KIND_ALIVE, ESTOP_ALIVE_OK, ESTOP_SOURCE_BRAIN, 3, 2);
        EstopNode_OnFrame(&node, 1, data, 8);
        Estop_Encode(data, ESTOP_KIND_RELEASE, 0, ESTOP_SOURCE_BRAIN, 3, 3);
        EstopNode_OnFrame(&node, 2, data, 8);
        CHECK(node.latched == 0 && EstopNode_Enable(&node) == 1 && node.drivers_enabled == 1, "ready and enabled");
        Estop_Encode(data, ESTOP_KIND_TRIP, reasons[i], ESTOP_SOURCE_CM5, 4, 4);
        EstopNode_OnFrame(&node, 3, data, 8);
        CHECK(node.latched == 1 && node.drivers_enabled == 0, "a trip latches and cuts the drivers");
    }
}

static void test_state_frame(void)
{
    EstopNode_t node;
    EstopNode_Reset(&node);
    uint8_t out[4];
    EstopNode_BuildState(&node, out);
    CHECK(out[0] == ESTOP_STATE_LATCHED && out[1] == 0 && out[2] == 0 && out[3] == 0, "power-up state");
    uint8_t data[8];
    Estop_Encode(data, ESTOP_KIND_TRIP, 1, ESTOP_SOURCE_BRAIN, 5, 1);
    EstopNode_OnFrame(&node, 0, data, 8);
    Estop_Encode(data, ESTOP_KIND_ALIVE, ESTOP_ALIVE_OK, ESTOP_SOURCE_BRAIN, 5, 0x0102);
    EstopNode_OnFrame(&node, 1, data, 8);
    Estop_Encode(data, ESTOP_KIND_RELEASE, 0, ESTOP_SOURCE_BRAIN, 5, 0x0103);
    EstopNode_OnFrame(&node, 2, data, 8);
    EstopNode_BuildState(&node, out);
    CHECK(out[0] == ESTOP_STATE_READY && out[1] == 5 && out[2] == 0x03 && out[3] == 0x01, "ready state carries epoch and last sequence");
}

#ifdef ESTOP_TEST_BRAIN
/* Several nodes (bit n = slot n) on one bus with a brain: the frames the brain sends are given to every node, the answers go back. */
typedef struct {
    EstopBrain_t brain;
    EstopNode_t nodes[8];
    uint8_t present;      /* bit n = a board is in slot n */
    uint8_t deaf;         /* bit n = that board hears nothing from the brain */
    uint32_t now;
    unsigned trips_sent, alives_sent, releases_sent;
} Bus;

static void bus_start(Bus *bus, uint8_t present)
{
    memset(bus, 0, sizeof *bus);
    bus->present = present;
    EstopBrain_Reset(&bus->brain, 0);
    for (unsigned i = 0; i < 8; i++) EstopNode_Reset(&bus->nodes[i]);
}

static void bus_step(Bus *bus)
{
    uint8_t frame[8];
    for (int guard = 0; guard < 4 && EstopBrain_Tick(&bus->brain, bus->now, frame); guard++) {
        if (frame[0] == ESTOP_KIND_TRIP) bus->trips_sent++;
        if (frame[0] == ESTOP_KIND_ALIVE) bus->alives_sent++;
        if (frame[0] == ESTOP_KIND_RELEASE) bus->releases_sent++;
        for (unsigned i = 0; i < 8; i++) {
            if (!(bus->present & (1u << i)) || (bus->deaf & (1u << i))) continue;
            EstopNode_OnFrame(&bus->nodes[i], bus->now, frame, 8);
            if (frame[0] == ESTOP_KIND_RELEASE) { /* a board answers every RELEASE with its state */
                uint8_t state[4];
                EstopNode_BuildState(&bus->nodes[i], state);
                EstopBrain_OnSlotState(&bus->brain, (uint8_t)i, state, 4);
            }
        }
    }
    for (unsigned i = 0; i < 8; i++) EstopNode_Tick(&bus->nodes[i], bus->now);
    bus->now++;
}

static void bus_run(Bus *bus, uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t++) bus_step(bus);
}

static int all_latched(const Bus *bus)
{
    for (unsigned i = 0; i < 8; i++) if ((bus->present & (1u << i)) && !bus->nodes[i].latched) return 0;
    return 1;
}

static int all_ready(const Bus *bus)
{
    for (unsigned i = 0; i < 8; i++) if ((bus->present & (1u << i)) && bus->nodes[i].latched) return 0;
    return 1;
}

static void test_brain_power_up_and_release(void)
{
    Bus bus;
    bus_start(&bus, 0x2D); /* slots 0, 2, 3 and 5 */
    bus_run(&bus, 100);
    CHECK(all_latched(&bus), "power-up: everybody latched");
    CHECK(bus.trips_sent >= 15 && bus.trips_sent <= 25, "a TRIP about every 5 ms while stopped");
    CHECK(bus.alives_sent >= 4 && bus.alives_sent <= 6, "ALIVE (tripped) about every 20 ms");
    CHECK(bus.releases_sent == 0, "no release unless a person asks");
    CHECK(bus.brain.tripped == 1, "brain stopped");

    CHECK(EstopBrain_RequestRelease(&bus.brain, bus.now, 0x2D) == 1, "release accepted");
    bus_run(&bus, 5);
    CHECK(all_ready(&bus), "every board READY after the release");
    CHECK(bus.brain.release_state == ESTOP_RELEASE_DONE && bus.brain.tripped == 0, "release done and acknowledged by every board");
    for (unsigned i = 0; i < 8; i++) CHECK(bus.nodes[i].drivers_enabled == 0, "release enables nothing");
    bus_run(&bus, 1000);
    CHECK(all_ready(&bus), "the ALIVE of a running brain keeps the boards READY");
    CHECK(bus.trips_sent < 40, "no TRIP while running");
    CHECK(EstopNode_Enable(&bus.nodes[0]) == 1 && EstopNode_Enable(&bus.nodes[2]) == 1, "the normal enable command works after a release");
}

static void test_brain_trip_then_epoch_and_release(void)
{
    Bus bus;
    bus_start(&bus, 0x03);
    bus_run(&bus, 50);
    CHECK(EstopBrain_RequestRelease(&bus.brain, bus.now, 0x03) == 1, "first release");
    bus_run(&bus, 5);
    CHECK(all_ready(&bus), "ready");
    const uint8_t epoch_before = bus.brain.epoch;
    EstopBrain_Trip(&bus.brain, bus.now, ESTOP_REASON_OPERATOR);
    bus_run(&bus, 2);
    CHECK(all_latched(&bus), "a stop from the brain latches every board within 2 ms");
    CHECK(bus.brain.epoch == (uint8_t)(epoch_before + 1u), "epoch counts the stop");
    bus_run(&bus, 50);
    CHECK(all_latched(&bus), "stays latched");
    CHECK(EstopBrain_RequestRelease(&bus.brain, bus.now, 0x03) == 1, "second release");
    bus_run(&bus, 5);
    CHECK(all_ready(&bus) && bus.brain.release_state == ESTOP_RELEASE_DONE, "second release acknowledged");
}

static void test_brain_release_fails_when_a_board_does_not_answer(void)
{
    Bus bus;
    bus_start(&bus, 0x0F);
    bus.deaf = 0x04; /* the board of slot 2 hears nothing */
    bus_run(&bus, 30);
    CHECK(EstopBrain_RequestRelease(&bus.brain, bus.now, 0x0F) == 1, "release started");
    bus_run(&bus, ESTOP_RELEASE_ACK_TIMEOUT_MS + 20u);
    CHECK(bus.brain.release_state == ESTOP_RELEASE_FAILED, "release cancelled after the wait");
    CHECK(bus.brain.missing_mask == 0x04, "the board that did not answer is named");
    CHECK(bus.brain.tripped == 1, "still stopped");
    bus_run(&bus, 20);
    CHECK(all_latched(&bus) || (bus.nodes[0].latched && bus.nodes[1].latched && bus.nodes[3].latched), "the boards that did hear are latched again");
}

static void test_brain_hears_a_trip_and_a_malformed_frame(void)
{
    Bus bus;
    bus_start(&bus, 0x01);
    bus_run(&bus, 20);
    EstopBrain_RequestRelease(&bus.brain, bus.now, 0x01);
    bus_run(&bus, 5);
    CHECK(bus.brain.tripped == 0, "running");
    const uint8_t epoch = bus.brain.epoch;
    uint8_t data[8];
    Estop_Encode(data, ESTOP_KIND_TRIP, ESTOP_REASON_CONTROLLER, 3, 0, 1);
    EstopBrain_OnFrame(&bus.brain, bus.now, data, 8);
    CHECK(bus.brain.tripped == 1 && bus.brain.epoch == (uint8_t)(epoch + 1u) && bus.brain.reason == ESTOP_REASON_CONTROLLER, "a TRIP of a board stops the brain");
    bus_step(&bus);
    CHECK(bus.nodes[0].latched == 1, "and the brain passes the stop on");

    EstopBrain_RequestRelease(&bus.brain, bus.now, 0x01);
    bus_run(&bus, 5);
    CHECK(bus.brain.tripped == 0, "running again");
    const uint8_t epoch2 = bus.brain.epoch;
    EstopBrain_OnFrame(&bus.brain, bus.now, data, 7); /* seven bytes */
    CHECK(bus.brain.tripped == 1 && bus.brain.epoch == (uint8_t)(epoch2 + 1u), "a malformed frame is a TRIP");
}

static void test_brain_trip_during_release_cancels_it(void)
{
    Bus bus;
    bus_start(&bus, 0x01);
    bus.deaf = 0x01;
    bus_run(&bus, 10);
    EstopBrain_RequestRelease(&bus.brain, bus.now, 0x01);
    bus_run(&bus, 30);
    CHECK(bus.brain.release_state == ESTOP_RELEASE_PENDING, "pending");
    EstopBrain_Trip(&bus.brain, bus.now, ESTOP_REASON_OPERATOR);
    CHECK(bus.brain.release_state == ESTOP_RELEASE_NONE && bus.brain.tripped == 1, "a TRIP cancels a release in progress");
    const unsigned releases = bus.releases_sent;
    bus_run(&bus, 100);
    CHECK(bus.releases_sent == releases, "no RELEASE is sent after the cancel");
    CHECK(EstopBrain_RequestRelease(&bus.brain, bus.now, 0x01) == 1 && EstopBrain_RequestRelease(&bus.brain, bus.now, 0x01) == 0, "one release at a time");
}

static void test_brain_without_boards_releases_at_once(void)
{
    Bus bus;
    bus_start(&bus, 0x00);
    bus_run(&bus, 10);
    EstopBrain_RequestRelease(&bus.brain, bus.now, 0x00);
    bus_run(&bus, 3);
    CHECK(bus.brain.release_state == ESTOP_RELEASE_DONE, "nothing to wait for");
}

static void test_brain_time_wraps(void)
{
    EstopBrain_t brain;
    EstopBrain_Reset(&brain, 0xFFFFFFF0u);
    uint8_t frame[8];
    unsigned sent = 0;
    for (uint32_t t = 0xFFFFFFF0u; t != 0x10u; t++) {
        while (EstopBrain_Tick(&brain, t, frame)) sent++;
    }
    CHECK(sent >= 8, "frames keep coming across the wrap of the millisecond counter");
}
#endif

int main(void)
{
    test_frames();
    test_sequence_order();
    test_scenarios();
    test_a_trip_cuts_the_drivers_in_every_state();
    test_state_frame();
#ifdef ESTOP_TEST_BRAIN
    test_brain_power_up_and_release();
    test_brain_trip_then_epoch_and_release();
    test_brain_release_fails_when_a_board_does_not_answer();
    test_brain_hears_a_trip_and_a_malformed_frame();
    test_brain_trip_during_release_cancels_it();
    test_brain_without_boards_releases_at_once();
    test_brain_time_wraps();
#endif
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("estop_can: all checks passed\n");
    return 0;
}
