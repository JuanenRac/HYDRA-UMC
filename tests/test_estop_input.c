/* =============================================================================
 * HYDRA-UMC Firmware - host test of estop_input.h (physical stop input, stop category per axis) together with the node of estop_can.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * ===========================================================================*/
#include <stdio.h>
#include <string.h>
#include "estop_can.h"
#include "estop_input.h"

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } } while (0)

static const EstopInputConfig_t NC_TO_GROUND = {1, 0, 2, 50}; /* healthy = 0 (closed to ground, pull up) */
static const EstopInputConfig_t NC_TO_SUPPLY = {1, 1, 2, 50};

static void test_fail_safe_start(void)
{
    EstopInput_t in;
    EstopInput_Init(&in, &NC_TO_GROUND);
    CHECK(in.tripped == 1, "starts tripped");
    CHECK(EstopInput_Sample(&in, 0, 0) == 1, "healthy level alone does not clear at once");
    CHECK(EstopInput_Sample(&in, 49, 0) == 1, "still tripped before the clear debounce");
    CHECK(EstopInput_Sample(&in, 50, 0) == 0, "clears after the clear debounce");
}

static void test_cut_wire_trips(void)
{
    EstopInput_t in;
    EstopInput_Init(&in, &NC_TO_GROUND);
    EstopInput_Sample(&in, 0, 0);
    EstopInput_Sample(&in, 60, 0);
    CHECK(in.tripped == 0, "healthy");
    /* a cut wire is pulled up: level 1 is the stop level */
    EstopInput_Sample(&in, 100, 1);
    CHECK(in.tripped == 0, "not yet: the trip debounce runs");
    CHECK(EstopInput_Sample(&in, 102, 1) == 1, "trips after the trip debounce");
    CHECK(EstopInput_PullUp(&NC_TO_GROUND) == 1, "healthy 0 -> pull up");
    CHECK(EstopInput_PullUp(&NC_TO_SUPPLY) == 0, "healthy 1 -> pull down");
}

static void test_polarity_from_configuration(void)
{
    EstopInput_t in;
    EstopInput_Init(&in, &NC_TO_SUPPLY);
    EstopInput_Sample(&in, 0, 1);
    EstopInput_Sample(&in, 60, 1);
    CHECK(in.tripped == 0, "level 1 is healthy with healthy_level 1");
    EstopInput_Sample(&in, 100, 0);
    CHECK(EstopInput_Sample(&in, 105, 0) == 1, "level 0 trips with healthy_level 1");
}

static void test_bounce_is_ignored(void)
{
    EstopInput_t in;
    EstopInput_Init(&in, &NC_TO_GROUND);
    EstopInput_Sample(&in, 0, 0);
    EstopInput_Sample(&in, 60, 0);
    /* a one millisecond glitch shorter than the trip debounce */
    EstopInput_Sample(&in, 100, 1);
    EstopInput_Sample(&in, 101, 0);
    CHECK(EstopInput_Sample(&in, 200, 0) == 0, "a glitch does not trip");
    /* a bounce on the way back to healthy restarts the clear time */
    EstopInput_Sample(&in, 300, 1);
    EstopInput_Sample(&in, 302, 1);
    CHECK(in.tripped == 1, "tripped");
    EstopInput_Sample(&in, 400, 0);
    EstopInput_Sample(&in, 430, 1);
    EstopInput_Sample(&in, 440, 0);
    CHECK(EstopInput_Sample(&in, 480, 0) == 1, "the clear debounce restarted after the bounce");
    CHECK(EstopInput_Sample(&in, 490, 0) == 0, "and ends 50 ms after the last bounce");
}

static void test_clock_wrap(void)
{
    EstopInput_t in;
    EstopInput_Init(&in, &NC_TO_GROUND);
    EstopInput_Sample(&in, 0xFFFFFFF0u, 0);
    CHECK(EstopInput_Sample(&in, 0xFFFFFFF0u + 60u, 0) == 0, "clears across the wrap of the clock");
}

static void test_disabled_input(void)
{
    const EstopInputConfig_t off = {0, 0, 2, 50};
    EstopInput_t in;
    EstopInput_Init(&in, &off);
    CHECK(EstopInput_Sample(&in, 0, 1) == 0, "a disabled input never trips");
}

static void test_category_zero_is_the_default(void)
{
    StopPolicy_t p;
    StopPolicy_Init(&p, 0x00u, 200u);
    CHECK(StopPolicy_Trip(&p, 1000) == 0x3Fu, "every axis is cut at once");
    CHECK(StopPolicy_DecelMask(&p) == 0, "nothing decelerates");
    CHECK(StopPolicy_Trip(&p, 1001) == 0, "a repeated stop cuts nothing new");
    CHECK(StopPolicy_Tick(&p, 5000) == 0, "no ramp to finish");
}

static void test_category_per_axis(void)
{
    StopPolicy_t p;
    StopPolicy_Init(&p, 0x24u, 200u); /* axes 3 and 6 are category 1 */
    CHECK(StopPolicy_Trip(&p, 1000) == 0x1Bu, "axes 1, 2, 4, 5 are cut at once");
    CHECK(StopPolicy_DecelMask(&p) == 0x24u, "axes 3 and 6 decelerate");
    CHECK(StopPolicy_Tick(&p, 1199) == 0, "not yet");
    CHECK(StopPolicy_Tick(&p, 1200) == 0x24u, "cut at the latest after ramp_max_ms");
    CHECK(StopPolicy_DecelMask(&p) == 0, "nothing left to decelerate");
    CHECK(p.cut_mask == 0x3Fu, "all cut");
    StopPolicy_Clear(&p);
    CHECK(p.active == 0 && p.cut_mask == 0, "cleared");
    CHECK(StopPolicy_Trip(&p, 9000) == 0x1Bu, "a new stop starts again");
}

static void test_category_mask_is_limited_to_six_axes(void)
{
    StopPolicy_t p;
    StopPolicy_Init(&p, 0xFFu, 10u);
    CHECK(p.category1_mask == 0x3Fu, "only six axes");
}

/* The stop input and the bus joined through the node: whichever fires first latches, and a RELEASE does not unlatch while the loop is open. */
static void feed_local_trip(EstopNode_t *n, uint32_t now, uint16_t seq)
{
    uint8_t f[8];
    Estop_Encode(f, ESTOP_KIND_TRIP, ESTOP_REASON_HARDWARE, 1u, n->epoch, seq);
    EstopNode_OnFrame(n, now, f, 8);
}

static void test_input_and_bus_share_one_latch(void)
{
    EstopNode_t n;
    uint8_t f[8];
    EstopFrame_t d;
    EstopNode_Reset(&n);
    /* the brain brings the node to READY: TRIP epoch 5 heard, ALIVE, RELEASE */
    Estop_Encode(f, ESTOP_KIND_TRIP, ESTOP_REASON_OPERATOR, ESTOP_SOURCE_BRAIN, 5, 1);
    EstopNode_OnFrame(&n, 0, f, 8);
    Estop_Encode(f, ESTOP_KIND_ALIVE, ESTOP_ALIVE_OK, ESTOP_SOURCE_BRAIN, 5, 2);
    EstopNode_OnFrame(&n, 10, f, 8);
    Estop_Encode(f, ESTOP_KIND_RELEASE, 0, ESTOP_SOURCE_BRAIN, 5, 3);
    EstopNode_OnFrame(&n, 20, f, 8);
    CHECK(n.latched == 0, "READY after the release");

    /* the input opens first: the same decision latches the node */
    feed_local_trip(&n, 30, 0);
    CHECK(n.latched == 1 && n.drivers_enabled == 0, "the physical input latches the node");

    /* a RELEASE arrives while the loop is still open: the firmware feeds the local trip again right after it */
    Estop_Encode(f, ESTOP_KIND_ALIVE, ESTOP_ALIVE_OK, ESTOP_SOURCE_BRAIN, 5, 4);
    EstopNode_OnFrame(&n, 40, f, 8);
    Estop_Encode(f, ESTOP_KIND_RELEASE, 0, ESTOP_SOURCE_BRAIN, 5, 5);
    EstopNode_OnFrame(&n, 50, f, 8);
    feed_local_trip(&n, 50, 1);
    CHECK(n.latched == 1, "a release cannot unlatch the board while the loop is open");
    CHECK(EstopNode_Enable(&n) == 0, "the drivers cannot be enabled");

    /* the other way: the bus fires first, the input is closed, the node stays latched until a release */
    Estop_Encode(f, ESTOP_KIND_TRIP, ESTOP_REASON_OPERATOR, ESTOP_SOURCE_BRAIN, 6, 6);
    EstopNode_OnFrame(&n, 60, f, 8);
    CHECK(n.latched == 1, "latched by the bus");
    Estop_Encode(f, ESTOP_KIND_ALIVE, ESTOP_ALIVE_OK, ESTOP_SOURCE_BRAIN, 6, 7);
    EstopNode_OnFrame(&n, 70, f, 8);
    Estop_Encode(f, ESTOP_KIND_RELEASE, 0, ESTOP_SOURCE_BRAIN, 6, 8);
    EstopNode_OnFrame(&n, 80, f, 8);
    CHECK(n.latched == 0, "with the loop closed the release works as before");

    /* the local trip frame is a well formed TRIP with reason 0x03 */
    Estop_Encode(f, ESTOP_KIND_TRIP, ESTOP_REASON_HARDWARE, 3u, n.epoch, 9);
    CHECK(Estop_Decode(f, 8, &d) == 1 && d.detail == ESTOP_REASON_HARDWARE, "reason 0x03 decodes");
}

int main(void)
{
    test_fail_safe_start();
    test_cut_wire_trips();
    test_polarity_from_configuration();
    test_bounce_is_ignored();
    test_clock_wrap();
    test_disabled_input();
    test_category_zero_is_the_default();
    test_category_per_axis();
    test_category_mask_is_limited_to_six_axes();
    test_input_and_bus_share_one_latch();
    if (failures) {
        printf("estop_input: %d FAILED\n", failures);
        return 1;
    }
    printf("estop_input: all passed\n");
    return 0;
}
