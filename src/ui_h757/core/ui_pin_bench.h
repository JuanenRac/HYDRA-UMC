/* =============================================================================
 * HYDRA-UMC Firmware - src/ui_h757/core/ui_pin_bench.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * The bench harness that measures what tools/ui_pin_cost.py only estimates: the cycles one PIN check takes on the Cortex-M7. It runs the PBKDF2 of a fixed PIN and salt with the
 * iterations given and returns the cycles that passed on a counter the board gives (on the H757 the DWT cycle counter, CYCCNT of the core). Header only, HAL-free: the host test
 * gives it a counter that charges a fixed number of cycles per compression, to check the arithmetic of the harness; on the board the number it prints is the measurement that
 * replaces pin_cycles_per_block of config/ui.cfg (divide by the compressions: 2 x iterations + 2). Nothing here has run on a board.
 */
#ifndef UI_PIN_BENCH_H
#define UI_PIN_BENCH_H

#include "ui_pin.h"

typedef struct {
    uint32_t (*cycles)(void *context); /* a free-running counter of core cycles that wraps at 32 bits */
    void *context;
} ui_cycle_counter_t;

/* The cycles of one PBKDF2-HMAC-SHA256 of the 4-digit PIN "4821" with a fixed salt (wrap-safe for a run shorter than one wrap of the counter). */
static inline uint32_t ui_pin_bench(const ui_cycle_counter_t *counter, uint32_t iterations, uint8_t out[32])
{
    static const uint8_t salt[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    uint32_t start = counter->cycles(counter->context);
    ui_pbkdf2_sha256((const uint8_t *)"4821", 4, salt, 16, iterations, out);
    return counter->cycles(counter->context) - start;
}

#endif /* UI_PIN_BENCH_H */
