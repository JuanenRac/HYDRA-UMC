/* =============================================================================
 * HYDRA-UMC Firmware - src/mcu_stm32h757/CM4/app_alive.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * app_alive.h - when the application of the CM4 may refresh its watchdog (HAL-free, host-testable). As it ships the watchdog is refreshed while the STACK A task
 * moves, so a hung SPI task would go unnoticed and the host would lose the link. With HYDRA_SPI_TASK_WATCHDOG=1 (off as it ships) a refresh also needs the SPI task
 * to have moved, once that task exists: either of the two standing still ends in the reset of the watchdog. tests/test_app_alive.c. Not run on a board.
 * ============================================================================= */
#ifndef APP_ALIVE_H
#define APP_ALIVE_H

#include <stdint.h>

#ifndef HYDRA_SPI_TASK_WATCHDOG
#define HYDRA_SPI_TASK_WATCHDOG 0
#endif

typedef struct {
    uint32_t seen_stack;
    uint32_t seen_spi;
} AppAlive_t;

/* `stack` and `spi` are the pass counters of the two tasks; `spi_required` is 0 until the SPI task was created. Returns 1 when the watchdog may be refreshed, and only
 * then remembers the counters: a task that stands still keeps answering 0. */
static inline uint8_t AppAlive_Look(AppAlive_t *alive, uint32_t stack, uint32_t spi, uint8_t spi_required)
{
    if (stack == alive->seen_stack) return 0u;
    if (spi_required != 0u && spi == alive->seen_spi) return 0u;
    alive->seen_stack = stack;
    alive->seen_spi = spi;
    return 1u;
}

#endif /* APP_ALIVE_H */
