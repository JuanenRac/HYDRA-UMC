/* =============================================================================
 * HYDRA-UMC - src/mcu_stm32h757/CM7/NorSpi5.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Driver of SPI5 as master towards the NOR flash of the H757 board (GD25Q128E), under the commands of Common/nor_flash.h. OFF by default: with HYDRA_NOR_SPI5 0 (as it ships)
 * NorSpi5.c compiles to an empty unit and no pin is touched. The pins are NOT in the platform profile (the netlist of the board is not in this repository), so the board set-up
 * passes them in the configuration: the port and pin of the chip select, the port of the bus and the pins of SCK, MISO and MOSI, and the alternate function. SPI mode 0, 8 bits, the
 * prescaler that gives the 6.25 Mbit/s of the profile from the 100 MHz kernel clock. Not run on a board: the proof there is a JEDEC identifier read and a write and read-back of a sector.
 * ========================================================================== */
#ifndef HYDRA_UMC_NOR_SPI5_H
#define HYDRA_UMC_NOR_SPI5_H

#ifndef HYDRA_NOR_SPI5
#define HYDRA_NOR_SPI5 0
#endif

#if HYDRA_NOR_SPI5

#include <stdint.h>
#include "nor_flash.h"
#include "stm32h7xx_hal.h"

typedef struct {
    GPIO_TypeDef *cs_port;        /* the chip select, driven by software (required) */
    uint16_t cs_pin;              /* exactly one pin */
    GPIO_TypeDef *bus_port;       /* the port of SCK, MISO and MOSI (required) */
    uint16_t bus_pins;            /* the mask of the pins of the bus (required) */
    uint32_t alternate;           /* the alternate function number of SPI5 on those pins */
    uint32_t transfer_timeout_ms; /* how long one transfer may take (1..1000) */
} NorSpi5Config;

/* Starts SPI5 and the chip select (high). Returns 1 when the configuration and the peripheral were accepted; on 0 the operations answer a failed transfer and move nothing. */
int NorSpi5_Init(const NorSpi5Config *cfg);

/* The operations for Common/nor_flash.h; they answer a failed transfer until a successful Init. */
const NorOps *NorSpi5_Ops(void);

#endif /* HYDRA_NOR_SPI5 */

#endif /* HYDRA_UMC_NOR_SPI5_H */
