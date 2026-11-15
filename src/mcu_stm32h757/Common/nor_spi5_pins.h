/* =============================================================================
 * HYDRA-UMC Firmware - src/mcu_stm32h757/Common/nor_spi5_pins.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * nor_spi5_pins.h - the pins of SPI5 towards the NOR flash (HAL-free, host-testable): the starting values of config/nor_spi5.cfg compiled in, and the checks the board set-up makes before it touches
 * a pin. A port is its index (0 for A ... 10 for K) and a pin its number (0..15). Starting values for the bench, not read from the netlist of the board. tools/boot_nor_stage.py reads the
 * file and the test of the repository keeps the two the same. Evidence: simulator.
 * ============================================================================= */
#ifndef HYDRA_UMC_NOR_SPI5_PINS_H
#define HYDRA_UMC_NOR_SPI5_PINS_H

#include <stdint.h>

#define NORS5_CS_PORT 5U /* F */
#define NORS5_CS_PIN 6U
#define NORS5_BUS_PORT 5U /* F */
#define NORS5_SCK_PIN 7U
#define NORS5_MISO_PIN 8U
#define NORS5_MOSI_PIN 9U
#define NORS5_ALTERNATE 5U
#define NORS5_TIMEOUT_MS 20U

#define NORS5_PORT_COUNT 11U /* GPIOA to GPIOK */
#define NORS5_TIMEOUT_MAX_MS 1000U

typedef struct {
    uint8_t cs_port, cs_pin, bus_port, sck_pin, miso_pin, mosi_pin, alternate;
    uint16_t timeout_ms;
} NorS5Pins;

static inline void NorS5_Defaults(NorS5Pins *p)
{
    p->cs_port = NORS5_CS_PORT;
    p->cs_pin = NORS5_CS_PIN;
    p->bus_port = NORS5_BUS_PORT;
    p->sck_pin = NORS5_SCK_PIN;
    p->miso_pin = NORS5_MISO_PIN;
    p->mosi_pin = NORS5_MOSI_PIN;
    p->alternate = NORS5_ALTERNATE;
    p->timeout_ms = NORS5_TIMEOUT_MS;
}

/* the mask of the three pins of the bus (the HAL wants them together) */
static inline uint16_t NorS5_BusMask(const NorS5Pins *p)
{
    return (uint16_t)((1U << p->sck_pin) | (1U << p->miso_pin) | (1U << p->mosi_pin));
}

/* 1 when the set is usable: ports and pins inside the chip, three different pins on the bus, the chip select not one of them when it is on the same port, a real alternate function and a timeout in range */
static inline int NorS5_Valid(const NorS5Pins *p)
{
    if (p == 0) return 0;
    if (p->cs_port >= NORS5_PORT_COUNT || p->bus_port >= NORS5_PORT_COUNT) return 0;
    if (p->cs_pin > 15U || p->sck_pin > 15U || p->miso_pin > 15U || p->mosi_pin > 15U || p->alternate > 15U) return 0;
    if (p->sck_pin == p->miso_pin || p->sck_pin == p->mosi_pin || p->miso_pin == p->mosi_pin) return 0;
    if (p->cs_port == p->bus_port && (NorS5_BusMask(p) & (1U << p->cs_pin)) != 0U) return 0;
    if (p->timeout_ms < 1U || p->timeout_ms > NORS5_TIMEOUT_MAX_MS) return 0;
    return 1;
}

#endif /* HYDRA_UMC_NOR_SPI5_PINS_H */
