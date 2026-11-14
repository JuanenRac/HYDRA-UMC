/* =============================================================================
 * HYDRA-UMC - src/mcu_stm32h757/CM7/FramI2c4.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * I2C4 on the CM7 (on the H757 board the CM7 owns I2C4, Common/boot_roles.h) as the bus of the F-RAM U7 of the main board: it starts the peripheral with the TIMINGR word of
 * Common/i2c_plan.h and gives CM4/fram_i2c.h the `transfer` function it talks to (the same one that CM4/SpiApp.c has for the H745 board, there bound to a hand-fixed timing). OFF by
 * default: with HYDRA_FRAM_I2C4_CM7 0 (as it ships) FramI2c4.c compiles to an empty unit and the pins are not touched. A timing word of 0 is refused (a made-up value could run the
 * bus out of specification). Not run on a board: bench rows 30 and 31 of docs/BENCH_TESTS.md remain.
 * ========================================================================== */
#ifndef HYDRA_UMC_FRAM_I2C4_H
#define HYDRA_UMC_FRAM_I2C4_H

#ifndef HYDRA_FRAM_I2C4_CM7
#define HYDRA_FRAM_I2C4_CM7 0
#endif

#if HYDRA_FRAM_I2C4_CM7

#include "stm32h7xx_hal.h"
#include "fram_i2c.h"

/* Starts I2C4 (SCL on PD12, SDA on PD13, alternate function 4, open drain, no internal pull: the pull-ups are on the board) with `timingr` and fills `bus` for FramI2c_Init.
 * `yield` (may be NULL) runs between two chunks. Returns 1 when the peripheral was accepted, 0 when the timing is 0, `bus` is missing or the HAL refuses the set-up. */
int FramI2c4_Open(uint32_t timingr, void (*yield)(void *context), void *context, FramI2cBus_t *bus);

/* The `transfer` of FramI2cBus_t over the HAL memory access with a 16-bit address. Returns 0 on success. */
int FramI2c4_Transfer(void *context, uint8_t address7, const uint8_t *tx, uint32_t tx_length, uint8_t *rx, uint32_t rx_length, uint32_t timeout_ms);

#endif /* HYDRA_FRAM_I2C4_CM7 */

#endif /* HYDRA_UMC_FRAM_I2C4_H */
