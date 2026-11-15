/* =============================================================================
 * HYDRA-UMC - src/mcu_stm32h757/CM7/I2cBuses.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * The three general I2C buses of the H757 board on the CM7: I2C1 (front panel and the I/O expander, SCL PB6, SDA PB7), I2C2 (the touch controller of the display, SCL PH4, SDA PH5)
 * and I2C3 (the expansion cards, SCL PA8, SDA PC9), all on alternate function 4 with open drain and no internal pull (the pull-ups are on the board). Each bus is started with the TIMINGR
 * word of Common/i2c_plan.h worked out from config/i2c_buses.cfg and gives plain write, read and probe operations on 7-bit addresses; the devices on them (the expander, the touch
 * controller) are not driven here. OFF by default: with HYDRA_I2C_BUSES_CM7 0 (as it ships) I2cBuses.c compiles to an empty unit and no pin is touched. A timing word of 0 is refused.
 * Not run on a board: the check that remains is a scope on each bus and a device answering a probe.
 * ========================================================================== */
#ifndef HYDRA_UMC_I2C_BUSES_H
#define HYDRA_UMC_I2C_BUSES_H

#ifndef HYDRA_I2C_BUSES_CM7
#define HYDRA_I2C_BUSES_CM7 0
#endif

#if HYDRA_I2C_BUSES_CM7

#include <stdint.h>

#include "stm32h7xx_hal.h"

#define I2CB_BUS1 0u
#define I2CB_BUS2 1u
#define I2CB_BUS3 2u
#define I2CB_COUNT 3u

/* Starts bus `bus` with `timingr`. Returns 1 when the HAL accepted it; 0 for a bus that does not exist, a timing of 0 or a refusal of the HAL (the bus then stays closed). */
int I2cBuses_Open(unsigned bus, uint32_t timingr);

/* One write or read of `length` bytes (1..255) to the 7-bit address; Probe asks whether a device answers (one try). Return 0 on success and 1 on any failure, also for a bus that is
 * not open, an address over 0x7F, a missing buffer or a length of 0 or over 255. */
int I2cBuses_Write(unsigned bus, uint8_t address7, const uint8_t *data, uint16_t length, uint32_t timeout_ms);
int I2cBuses_Read(unsigned bus, uint8_t address7, uint8_t *data, uint16_t length, uint32_t timeout_ms);
int I2cBuses_Probe(unsigned bus, uint8_t address7, uint32_t timeout_ms);

#endif /* HYDRA_I2C_BUSES_CM7 */

#endif /* HYDRA_UMC_I2C_BUSES_H */
