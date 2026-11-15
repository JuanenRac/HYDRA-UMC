/* =============================================================================
 * HYDRA-UMC Firmware - tests/test_touch_gt911.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Host test of the touch input of the interface (src/mcu_stm32h757/Common/touch_gt911.h) against a double of the controller that plays the register file of a GT911 behind the two
 * operations of an I2C bus: the checks of the numbers, the mapping of a raw position (scale, swap, mirror, the edges, the refusal of a point outside the raw size), the probe, and the
 * poll in all its branches (no new frame, released, pressed, more points than the chip has, a point out of range, a bus that fails at every transfer). A frame that was read is always
 * acknowledged. The mapping gives the same numbers as tests/test_touch_gt911.py against the Python twin. Evidence: simulator.
 *
 *   cc -std=c11 -Wall -Wextra -Werror -Isrc/mcu_stm32h757/Common -o test_touch_gt911 tests/test_touch_gt911.c
 */
#include <stdio.h>
#include <string.h>

#include "touch_gt911.h"

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } } while (0)

static uint8_t regs[0x10000];
static unsigned pointer;
static int transfers, fail_at, acks, last_addr7;

static int d_write(void *ctx, uint8_t a, const uint8_t *d, uint16_t n)
{
    (void)ctx;
    last_addr7 = a;
    if (fail_at >= 0 && transfers++ == fail_at) return 1;
    if (n < 2U) return 1;
    pointer = (unsigned)((d[0] << 8) | d[1]);
    if (n > 2U) {
        memcpy(regs + pointer, d + 2, (size_t)n - 2U);
        if (pointer == GT911_REG_STATUS) acks++;
    }
    return 0;
}

static int d_read(void *ctx, uint8_t a, uint8_t *d, uint16_t n)
{
    (void)ctx;
    last_addr7 = a;
    if (fail_at >= 0 && transfers++ == fail_at) return 1;
    memcpy(d, regs + pointer, n);
    return 0;
}

static Gt911Bus bus = {d_write, d_read, 0};

static void reset(void)
{
    memset(regs, 0, sizeof regs);
    pointer = 0;
    transfers = 0;
    fail_at = -1;
    acks = 0;
}

static void frame(unsigned count, unsigned x, unsigned y)
{
    regs[GT911_REG_STATUS] = (uint8_t)(0x80U | count);
    regs[GT911_REG_POINT0] = 0;
    regs[GT911_REG_POINT0 + 1] = (uint8_t)(x & 0xFFU);
    regs[GT911_REG_POINT0 + 2] = (uint8_t)(x >> 8);
    regs[GT911_REG_POINT0 + 3] = (uint8_t)(y & 0xFFU);
    regs[GT911_REG_POINT0 + 4] = (uint8_t)(y >> 8);
}

int main(void)
{
    Gt911Config c = {GT911_ADDR_PRIMARY, 1280U, 720U, 1280U, 720U, 0U, 0U, 0U}, d;
    Gt911State st;
    TouchSample out;
    uint16_t x = 9, y = 9;
    int i;

    /* the numbers */
    CHECK(Gt911_Check(&c) == GT911_OK, "the starting values");
    CHECK(Gt911_Check(0) == GT911_E_ARG, "no configuration");
    d = c;
    d.address7 = GT911_ADDR_ALTERNATE;
    CHECK(Gt911_Check(&d) == GT911_OK, "the alternate address");
    d.address7 = 0x40U;
    CHECK(Gt911_Check(&d) == GT911_E_CONFIG, "another address");
    d = c;
    d.raw_width = 0U;
    CHECK(Gt911_Check(&d) == GT911_E_CONFIG, "raw width 0");
    d = c;
    d.raw_height = 2049U;
    CHECK(Gt911_Check(&d) == GT911_E_CONFIG, "raw height too big");
    d = c;
    d.raw_height = 0U;
    CHECK(Gt911_Check(&d) == GT911_E_CONFIG, "raw height 0");
    d = c;
    d.raw_width = 2049U;
    CHECK(Gt911_Check(&d) == GT911_E_CONFIG, "raw width too big");
    d = c;
    d.width = 0U;
    CHECK(Gt911_Check(&d) == GT911_E_CONFIG, "picture width 0");
    d = c;
    d.width = 2049U;
    CHECK(Gt911_Check(&d) == GT911_E_CONFIG, "picture width too big");
    d = c;
    d.height = 0U;
    CHECK(Gt911_Check(&d) == GT911_E_CONFIG, "picture height 0");
    d = c;
    d.height = 2049U;
    CHECK(Gt911_Check(&d) == GT911_E_CONFIG, "picture height too big");

    /* the mapping */
    CHECK(Gt911_Map(&c, 0U, 0U, &x, &y) == GT911_OK && x == 0U && y == 0U, "the first pixel");
    CHECK(Gt911_Map(&c, 1279U, 719U, &x, &y) == GT911_OK && x == 1279U && y == 719U, "the last pixel");
    CHECK(Gt911_Map(&c, 640U, 360U, &x, &y) == GT911_OK && x == 640U && y == 360U, "the middle");
    CHECK(Gt911_Map(&c, 1280U, 0U, &x, &y) == GT911_E_RANGE && Gt911_Map(&c, 0U, 720U, &x, &y) == GT911_E_RANGE, "a point outside the raw size");
    CHECK(Gt911_Map(0, 0U, 0U, &x, &y) == GT911_E_ARG, "mapping without configuration");
    CHECK(Gt911_Map(&c, 0U, 0U, 0, &y) == GT911_E_ARG && Gt911_Map(&c, 0U, 0U, &x, 0) == GT911_E_ARG, "mapping without answer");
    d = c;
    d.mirror_x = 1U;
    CHECK(Gt911_Map(&d, 0U, 0U, &x, &y) == GT911_OK && x == 1279U && y == 0U, "mirror x");
    d = c;
    d.mirror_y = 1U;
    CHECK(Gt911_Map(&d, 0U, 0U, &x, &y) == GT911_OK && x == 0U && y == 719U, "mirror y");
    d = c;
    d.raw_width = 720U;
    d.raw_height = 1280U;
    d.swap_xy = 1U;
    CHECK(Gt911_Map(&d, 0U, 1279U, &x, &y) == GT911_OK && x == 1279U && y == 0U, "swap: the long raw axis becomes x");
    CHECK(Gt911_Map(&d, 719U, 0U, &x, &y) == GT911_OK && x == 0U && y == 719U, "swap: the short raw axis becomes y");
    d = c;
    d.raw_width = 640U;
    d.raw_height = 360U;
    CHECK(Gt911_Map(&d, 639U, 359U, &x, &y) == GT911_OK && x == 1278U && y == 718U, "a smaller raw size is scaled up and rounded down");

    /* the probe */
    reset();
    memcpy(regs + GT911_REG_ID, "911\0", 4);
    CHECK(Gt911_Probe(&bus, &c) == GT911_OK && last_addr7 == GT911_ADDR_PRIMARY, "a GT911 answers");
    regs[GT911_REG_ID + 3] = 1;
    CHECK(Gt911_Probe(&bus, &c) == GT911_E_ID, "another chip");
    CHECK(Gt911_Probe(0, &c) == GT911_E_ARG && Gt911_Probe(&bus, 0) == GT911_E_ARG, "probe without bus or configuration");
    {
        Gt911Bus half = {d_write, 0, 0};
        CHECK(Gt911_Probe(&half, &c) == GT911_E_ARG, "a bus without a read");
        half.read = d_read;
        half.write = 0;
        CHECK(Gt911_Probe(&half, &c) == GT911_E_ARG, "a bus without a write");
    }
    for (i = 0; i < 2; i++) {
        fail_at = i;
        transfers = 0;
        CHECK(Gt911_Probe(&bus, &c) == GT911_E_BUS, "a bus that fails during the probe");
    }

    /* the poll */
    reset();
    memset(&st, 0, sizeof st);
    CHECK(Gt911_Poll(&bus, &c, &st, &out) == GT911_NODATA && out.pressed == 0U, "no new frame");
    CHECK(acks == 0, "nothing to acknowledge");
    frame(1U, 100U, 200U);
    CHECK(Gt911_Poll(&bus, &c, &st, &out) == GT911_OK && out.pressed == 1U && out.x == 100U && out.y == 200U, "pressed");
    CHECK(acks == 1 && regs[GT911_REG_STATUS] == 0U, "and acknowledged");
    CHECK(Gt911_Poll(&bus, &c, &st, &out) == GT911_NODATA && out.pressed == 1U && out.x == 100U && out.y == 200U, "no new frame: the press is held");
    frame(2U, 1279U, 719U);
    CHECK(Gt911_Poll(&bus, &c, &st, &out) == GT911_OK && out.x == 1279U && out.y == 719U, "with two points the first one is used");
    frame(0U, 0U, 0U);
    CHECK(Gt911_Poll(&bus, &c, &st, &out) == GT911_OK && out.pressed == 0U && out.x == 1279U && out.y == 719U, "released keeps the last position");
    CHECK(acks == 3, "and acknowledged");
    frame(6U, 5U, 5U);
    CHECK(Gt911_Poll(&bus, &c, &st, &out) == GT911_E_COUNT && acks == 4 && out.pressed == 0U, "more points than the chip has");
    frame(1U, 1280U, 10U);
    CHECK(Gt911_Poll(&bus, &c, &st, &out) == GT911_E_RANGE && acks == 5 && out.pressed == 0U, "a point out of range is refused and acknowledged");
    frame(1U, 0x0105U, 0x0102U);
    CHECK(Gt911_Poll(&bus, &c, &st, &out) == GT911_OK && out.x == 0x0105U && out.y == 0x0102U, "the high bytes of the position");
    CHECK(Gt911_Poll(0, &c, &st, &out) == GT911_E_ARG && Gt911_Poll(&bus, 0, &st, &out) == GT911_E_ARG, "poll without bus or configuration");
    CHECK(Gt911_Poll(&bus, &c, 0, &out) == GT911_E_ARG && Gt911_Poll(&bus, &c, &st, 0) == GT911_E_ARG, "poll without state or answer");
    {
        Gt911Bus none = {0, 0, 0};
        CHECK(Gt911_Poll(&none, &c, &st, &out) == GT911_E_ARG, "poll with a bus that does nothing");
    }
    d = c;
    d.address7 = 0x40U;
    CHECK(Gt911_Poll(&bus, &d, &st, &out) == GT911_E_CONFIG, "poll with a bad address");

    /* a bus that fails at every transfer of a pressed frame: status (2), point (2), mapping, acknowledge (1) */
    for (i = 0; i < 5; i++) {
        reset();
        memset(&st, 0, sizeof st);
        frame(1U, 10U, 20U);
        fail_at = i;
        transfers = 0;
        CHECK(Gt911_Poll(&bus, &c, &st, &out) == GT911_E_BUS, "a bus that fails during the poll");
        CHECK(st.last.pressed == 0U || i == 4, "no sample is taken from a failed read");
    }
    reset();
    memset(&st, 0, sizeof st);
    frame(0U, 0U, 0U);
    fail_at = 2;
    transfers = 0;
    CHECK(Gt911_Poll(&bus, &c, &st, &out) == GT911_E_BUS, "a released frame whose acknowledge fails");

    if (failures == 0) printf("touch_gt911 host logic tests passed\n");
    return failures == 0 ? 0 : 1;
}
