/* =============================================================================
 * HYDRA-UMC - tests/test_app_alive.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * The rule of the watchdog refresh of the CM4 application (app_alive.h), with the two tasks simulated by their pass counters: a task that hangs stops the refreshes
 * for good. Evidence: simulator.
 * ========================================================================== */
#include <assert.h>
#include <stdio.h>
#include "app_alive.h"

int main(void)
{
    AppAlive_t alive = { 0u, 0u };
    uint32_t stack = 0u, spi = 0u;

    assert(AppAlive_Look(&alive, stack, spi, 0u) == 0u); /* nothing has run yet */
    assert(AppAlive_Look(&alive, ++stack, spi, 0u) == 1u); /* no SPI task yet: STACK A is enough */
    assert(AppAlive_Look(&alive, stack, ++spi, 1u) == 0u); /* STACK A hung */

    for (int look = 0; look < 100; look++) assert(AppAlive_Look(&alive, ++stack, ++spi, 1u) == 1u);

    /* the SPI task hangs while STACK A goes on: never a refresh again */
    for (int look = 0; look < 100; look++) assert(AppAlive_Look(&alive, ++stack, spi, 1u) == 0u);
    assert(AppAlive_Look(&alive, ++stack, ++spi, 1u) == 1u); /* and it comes back if it moves before the watchdog bites */

    /* the counters wrap around */
    alive.seen_stack = 0xFFFFFFFFu;
    alive.seen_spi = 0xFFFFFFFFu;
    assert(AppAlive_Look(&alive, 0u, 0u, 1u) == 1u);

    puts("app_alive: ok");
    return 0;
}
