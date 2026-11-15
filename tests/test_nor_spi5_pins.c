/* =============================================================================
 * HYDRA-UMC Firmware - tests/test_nor_spi5_pins.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Host test of the pins of SPI5 towards the NOR flash (src/mcu_stm32h757/Common/nor_spi5_pins.h): the starting values are valid, the mask of the bus is the three pins, and every way a set
 * of pins can be wrong is refused (a port or pin outside the chip, two pins of the bus the same, the chip select on a pin of the bus, an alternate function that does not exist, a timeout out
 * of range). Evidence: simulator.
 *
 *   cc -std=c11 -Wall -Wextra -Werror -Isrc/mcu_stm32h757/Common -o test_nor_spi5_pins tests/test_nor_spi5_pins.c
 */
#include <stdio.h>

#include "nor_spi5_pins.h"

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } } while (0)

int main(void)
{
    NorS5Pins p, q;

    NorS5_Defaults(&p);
    CHECK(p.cs_port == NORS5_CS_PORT && p.cs_pin == NORS5_CS_PIN && p.bus_port == NORS5_BUS_PORT, "defaults: ports and chip select");
    CHECK(p.sck_pin == NORS5_SCK_PIN && p.miso_pin == NORS5_MISO_PIN && p.mosi_pin == NORS5_MOSI_PIN, "defaults: the bus");
    CHECK(p.alternate == NORS5_ALTERNATE && p.timeout_ms == NORS5_TIMEOUT_MS, "defaults: alternate function and timeout");
    CHECK(NorS5_Valid(&p), "the starting values are valid");
    CHECK(NorS5_BusMask(&p) == (uint16_t)((1U << 7) | (1U << 8) | (1U << 9)), "the mask of the bus is its three pins");
    CHECK(!NorS5_Valid(0), "null");

    q = p;
    q.cs_port = NORS5_PORT_COUNT;
    CHECK(!NorS5_Valid(&q), "a chip select port outside the chip");
    q = p;
    q.bus_port = NORS5_PORT_COUNT;
    CHECK(!NorS5_Valid(&q), "a bus port outside the chip");
    q = p;
    q.cs_port = NORS5_PORT_COUNT - 1U;
    CHECK(NorS5_Valid(&q), "the last port is inside the chip");
    q = p;
    q.cs_pin = 16;
    CHECK(!NorS5_Valid(&q), "a chip select pin above 15");
    q = p;
    q.sck_pin = 16;
    CHECK(!NorS5_Valid(&q), "a clock pin above 15");
    q = p;
    q.miso_pin = 16;
    CHECK(!NorS5_Valid(&q), "an input pin above 15");
    q = p;
    q.mosi_pin = 16;
    CHECK(!NorS5_Valid(&q), "an output pin above 15");
    q = p;
    q.alternate = 16;
    CHECK(!NorS5_Valid(&q), "an alternate function above 15");
    q = p;
    q.miso_pin = q.sck_pin;
    CHECK(!NorS5_Valid(&q), "clock and input on the same pin");
    q = p;
    q.mosi_pin = q.sck_pin;
    CHECK(!NorS5_Valid(&q), "clock and output on the same pin");
    q = p;
    q.mosi_pin = q.miso_pin;
    CHECK(!NorS5_Valid(&q), "input and output on the same pin");
    q = p;
    q.cs_pin = q.sck_pin;
    CHECK(!NorS5_Valid(&q), "the chip select on a pin of the bus");
    q = p;
    q.cs_pin = q.sck_pin;
    q.cs_port = 0;
    CHECK(NorS5_Valid(&q), "the same pin number on another port is fine");
    q = p;
    q.timeout_ms = 0;
    CHECK(!NorS5_Valid(&q), "no timeout");
    q = p;
    q.timeout_ms = NORS5_TIMEOUT_MAX_MS + 1U;
    CHECK(!NorS5_Valid(&q), "a timeout above the limit");
    q = p;
    q.timeout_ms = NORS5_TIMEOUT_MAX_MS;
    CHECK(NorS5_Valid(&q), "the limit itself");

    if (failures == 0) printf("nor_spi5_pins host logic tests passed\n");
    return failures == 0 ? 0 : 1;
}
