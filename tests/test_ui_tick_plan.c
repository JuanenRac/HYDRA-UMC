/* =============================================================================
 * HYDRA-UMC Firmware - tests/test_ui_tick_plan.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Host test of the tick and the pace of the interface library (src/mcu_stm32h757/Common/ui_tick_plan.h): the checks of the numbers, the milliseconds fed (also across the wrap of
 * the counter), the stall that is held to the catch-up, and the sleep held between its bounds (the same numbers as tests/test_ui_tick_plan.py against the Python twin). A sweep
 * checks that the amounts fed over a run without stalls add up to the time that went by. Evidence: simulator.
 *
 *   cc -std=c11 -Wall -Wextra -Werror -Isrc/mcu_stm32h757/Common -o test_ui_tick_plan tests/test_ui_tick_plan.c
 */
#include <stdio.h>

#include "ui_tick_plan.h"

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } } while (0)

int main(void)
{
    UtpConfig c = {100UL, 5UL, 33UL}, bad;
    UtpState s;
    uint32_t feed = 99UL, now, total = 0UL, i;

    /* the numbers */
    CHECK(Utp_Check(&c) == UTP_OK, "the starting values");
    CHECK(Utp_Check(0) == UTP_E_ARG, "no configuration");
    bad = c;
    bad.handler_min_ms = 0UL;
    CHECK(Utp_Check(&bad) == UTP_E_CONFIG, "a minimum of 0");
    bad = c;
    bad.handler_max_ms = 1001UL;
    CHECK(Utp_Check(&bad) == UTP_E_CONFIG, "a maximum above 1000");
    bad = c;
    bad.handler_min_ms = 34UL;
    CHECK(Utp_Check(&bad) == UTP_E_CONFIG, "a minimum above the maximum");
    bad = c;
    bad.handler_min_ms = bad.handler_max_ms = 33UL;
    CHECK(Utp_Check(&bad) == UTP_OK, "equal bounds");
    bad = c;
    bad.catchup_max_ms = 0UL;
    CHECK(Utp_Check(&bad) == UTP_E_CONFIG, "a catch-up of 0");
    bad.catchup_max_ms = 10001UL;
    CHECK(Utp_Check(&bad) == UTP_E_CONFIG, "a catch-up above 10000");
    bad.catchup_max_ms = 10000UL;
    CHECK(Utp_Check(&bad) == UTP_OK, "a catch-up of 10000");

    /* init and tick */
    CHECK(Utp_Init(0, 0UL) == UTP_E_ARG, "init without state");
    CHECK(Utp_Init(&s, 1000UL) == UTP_OK && s.last_ms == 1000UL, "init");
    CHECK(Utp_Tick(&s, &c, 1000UL, &feed) == UTP_OK && feed == 0UL, "nothing went by");
    CHECK(Utp_Tick(&s, &c, 1005UL, &feed) == UTP_OK && feed == 5UL && s.last_ms == 1005UL, "five went by");
    CHECK(Utp_Tick(&s, &c, 1105UL, &feed) == UTP_OK && feed == 100UL, "exactly the catch-up is not a stall");
    CHECK(Utp_Tick(&s, &c, 1206UL, &feed) == UTP_CLAMPED && feed == 100UL && s.last_ms == 1206UL, "a stall is held to the catch-up and not owed");
    CHECK(Utp_Tick(&s, &c, 1207UL, &feed) == UTP_OK && feed == 1UL, "the next tick is normal again");
    CHECK(Utp_Tick(&s, 0, 1300UL, &feed) == UTP_E_ARG, "no configuration to tick");
    bad = c;
    bad.handler_min_ms = 0UL;
    CHECK(Utp_Tick(&s, &bad, 1300UL, &feed) == UTP_E_CONFIG, "a bad configuration to tick");
    CHECK(Utp_Tick(0, &c, 1300UL, &feed) == UTP_E_ARG && Utp_Tick(&s, &c, 1300UL, 0) == UTP_E_ARG, "no state or no answer");

    /* the wrap of the counter */
    Utp_Init(&s, 0xFFFFFFF0UL);
    CHECK(Utp_Tick(&s, &c, 0x00000004UL, &feed) == UTP_OK && feed == 20UL, "twenty ms across the wrap");

    /* the sleep */
    CHECK(Utp_SleepMs(&c, 0UL) == 5UL && Utp_SleepMs(&c, 4UL) == 5UL, "below the minimum");
    CHECK(Utp_SleepMs(&c, 5UL) == 5UL && Utp_SleepMs(&c, 20UL) == 20UL && Utp_SleepMs(&c, 33UL) == 33UL, "inside the bounds");
    CHECK(Utp_SleepMs(&c, 34UL) == 33UL && Utp_SleepMs(&c, UTP_NO_TIMER_READY) == 33UL, "above the maximum, and no timer ready");

    /* a run without stalls adds up to the time that went by, whatever the step */
    for (i = 1UL; i <= 100UL; i++) {
        uint32_t step = 1UL + (i * 7UL) % 50UL;
        Utp_Init(&s, 0xFFFFFF00UL + i);
        now = 0xFFFFFF00UL + i;
        total = 0UL;
        {
            uint32_t k, want = 0UL;
            for (k = 0UL; k < 40UL; k++) {
                now += step;
                want += step;
                (void)Utp_Tick(&s, &c, now, &feed);
                total += feed;
            }
            CHECK(total == want, "the amounts fed add up");
        }
    }

    if (failures == 0) printf("ui_tick_plan host logic tests passed\n");
    return failures == 0 ? 0 : 1;
}
