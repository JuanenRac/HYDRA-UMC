/* =============================================================================
 * HYDRA-UMC Firmware - tests/test_nor_spi5.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Host test of the SPI5 driver of the NOR flash (src/mcu_stm32h757/CM7/NorSpi5.c) against a double of the HAL (tests/stubs/h7spi5). The double plays the pins and the peripheral AND a
 * 25-series NOR chip behind them (JEDEC identifier, status, write enable, read, page program, sector erase, a program only clears bits), so the driver is run end to end with the commands
 * of Common/nor_flash.h: the identifier is read, a sector is erased, a range is written across a page boundary with read-back and read again. It checks the configuration given to the
 * peripheral (master, 8 bits, mode 0, prescaler 16, software chip select), the idle level of the chip select, the refusal of a bad configuration, the failure of a call before the start,
 * the abort of a transfer that failed and the three transfer directions. Evidence: simulator.
 *
 *   cc -std=c11 -Wall -Wextra -Werror -DHYDRA_PLATFORM_H757 -DHYDRA_NOR_SPI5=1 -Itests/stubs/h7spi5 -Isrc/mcu_stm32h757/CM7 -Isrc/mcu_stm32h757/Common \
 *      -o test_nor_spi5 tests/test_nor_spi5.c src/mcu_stm32h757/CM7/NorSpi5.c
 */
#include <stdio.h>
#include <string.h>

#include "stm32h7xx_hal.h"
#include "NorSpi5.h"

#if !HYDRA_NOR_SPI5
#error "this test is for the driver with its switch on: build it with -DHYDRA_NOR_SPI5=1"
#endif

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } } while (0)

SPI_TypeDef stub_spi5 = {5};
static GPIO_TypeDef port_a = {1}, port_b = {2};

static int clock_enabled, init_calls, spi_init_calls, spi_init_fail, aborts, transfer_fail;
static GPIO_InitTypeDef init_cs, init_bus;
static int cs_level = -1; /* -1: never driven */
static SPI_InitTypeDef spi_seen;
static uint32_t timeout_seen;
static int tx_rx_calls, tx_only_calls, rx_only_calls;

/* the chip */
#define CHIP_BYTES (64u * 1024u)
static uint8_t chip[CHIP_BYTES];
static uint8_t status_reg;
static int wip_polls;
static uint8_t command[260];
static uint32_t command_len;

void stub_clock_enable(int which) { clock_enabled |= which; }

void HAL_GPIO_Init(GPIO_TypeDef *port, GPIO_InitTypeDef *init)
{
    init_calls++;
    if (init->Mode == GPIO_MODE_AF_PP) {
        init_bus = *init;
        CHECK(port == &port_b, "the bus pins are initialised on the bus port");
    } else {
        init_cs = *init;
        CHECK(port == &port_a, "the chip select is initialised on its port");
    }
}

void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState state)
{
    CHECK(port == &port_a && pin == (1u << 4), "only the chip select is driven");
    if (state == GPIO_PIN_SET && cs_level == 0) command_len = 0; /* deselected: the command is over */
    if (state == GPIO_PIN_RESET) command_len = 0;
    cs_level = (int)state;
}

HAL_StatusTypeDef HAL_SPI_Init(SPI_HandleTypeDef *hspi)
{
    spi_init_calls++;
    spi_seen = hspi->Init;
    return spi_init_fail ? HAL_ERROR : HAL_OK;
}

static uint32_t address_of(void) { return ((uint32_t)command[1] << 16) | ((uint32_t)command[2] << 8) | command[3]; }

/* bytes the master sends */
static void chip_receives(const uint8_t *tx, uint16_t size)
{
    for (uint16_t i = 0; i < size; i++) {
        command[command_len++] = tx[i];
        if (command_len == 1u && tx[i] == 0x06u) {
            status_reg |= 0x02u;
        } else if (command_len == 4u && command[0] == 0x20u) {
            if (status_reg & 0x02u) {
                memset(chip + (address_of() % CHIP_BYTES & ~0xFFFu), 0xFF, 4096u);
                status_reg &= (uint8_t)~0x02u;
                wip_polls = 2;
            }
        } else if (command_len > 4u && command[0] == 0x02u) {
            if (status_reg & 0x02u) {
                /* the page wraps inside its 256-byte page; a program only clears bits */
                const uint32_t base = address_of();
                const uint32_t offset = (base & ~0xFFu) | ((base + (command_len - 5u)) & 0xFFu);
                chip[offset % CHIP_BYTES] &= tx[i];
            }
            if (i + 1u == size) {
                status_reg &= (uint8_t)~0x02u;
                wip_polls = 1;
            }
        }
    }
}

/* bytes the chip answers */
static void chip_sends(uint8_t *rx, uint16_t size)
{
    for (uint16_t i = 0; i < size; i++) {
        if (command[0] == 0x9Fu) {
            static const uint8_t id[3] = {0xC8, 0x40, 0x18};
            rx[i] = id[i % 3u];
        } else if (command[0] == 0x05u) {
            rx[i] = status_reg | (uint8_t)(wip_polls > 0 ? 1u : 0u);
            if (wip_polls > 0) wip_polls--;
        } else if (command[0] == 0x03u) {
            rx[i] = chip[(address_of() + i) % CHIP_BYTES];
        } else {
            rx[i] = 0xEE;
        }
    }
}

HAL_StatusTypeDef HAL_SPI_TransmitReceive(SPI_HandleTypeDef *hspi, uint8_t *tx, uint8_t *rx, uint16_t size, uint32_t timeout)
{
    (void)hspi;
    tx_rx_calls++;
    timeout_seen = timeout;
    if (transfer_fail) return HAL_TIMEOUT;
    chip_receives(tx, size);
    chip_sends(rx, size);
    return HAL_OK;
}

HAL_StatusTypeDef HAL_SPI_Transmit(SPI_HandleTypeDef *hspi, uint8_t *tx, uint16_t size, uint32_t timeout)
{
    (void)hspi;
    tx_only_calls++;
    timeout_seen = timeout;
    if (transfer_fail) return HAL_TIMEOUT;
    chip_receives(tx, size);
    return HAL_OK;
}

HAL_StatusTypeDef HAL_SPI_Receive(SPI_HandleTypeDef *hspi, uint8_t *rx, uint16_t size, uint32_t timeout)
{
    (void)hspi;
    rx_only_calls++;
    timeout_seen = timeout;
    if (transfer_fail) return HAL_TIMEOUT;
    chip_sends(rx, size);
    return HAL_OK;
}

HAL_StatusTypeDef HAL_SPI_Abort(SPI_HandleTypeDef *hspi)
{
    (void)hspi;
    aborts++;
    return HAL_OK;
}

int main(void)
{
    const NorSpi5Config good = {&port_a, (uint16_t)(1u << 4), &port_b, (uint16_t)((1u << 1) | (1u << 2) | (1u << 3)), 5u, 20u};
    const NorOps *ops = NorSpi5_Ops();
    uint8_t buf[600], scratch[HYDRA_NOR_PAGE_BYTES], back[600];
    uint32_t id = 0;
    NorSpi5Config bad;

    memset(chip, 0xFF, sizeof chip);

    /* before a successful start nothing moves */
    CHECK(ops != NULL && ops->select != NULL && ops->transfer != NULL, "the operations exist");
    ops->select(NULL, 1); /* no port yet: ignored */
    CHECK(init_calls == 0 && cs_level == -1, "nothing was driven before the start");
    CHECK(ops->transfer(NULL, buf, NULL, 1u) == 0, "a transfer before the start is refused");
    CHECK(Nor_ReadId(ops, NULL, &id) == NOR_ERR_TRANSFER, "the identifier cannot be read before the start");

    /* a bad configuration is refused and touches nothing */
    CHECK(NorSpi5_Init(NULL) == 0, "no configuration");
    bad = good; bad.cs_port = NULL;
    CHECK(NorSpi5_Init(&bad) == 0, "no chip select port");
    bad = good; bad.bus_port = NULL;
    CHECK(NorSpi5_Init(&bad) == 0, "no bus port");
    bad = good; bad.bus_pins = 0u;
    CHECK(NorSpi5_Init(&bad) == 0, "no bus pins");
    bad = good; bad.cs_pin = 0u;
    CHECK(NorSpi5_Init(&bad) == 0, "no chip select pin");
    bad = good; bad.cs_pin = (uint16_t)((1u << 4) | (1u << 5));
    CHECK(NorSpi5_Init(&bad) == 0, "two chip select pins");
    bad = good; bad.transfer_timeout_ms = 0u;
    CHECK(NorSpi5_Init(&bad) == 0, "time-out 0");
    bad.transfer_timeout_ms = 1001u;
    CHECK(NorSpi5_Init(&bad) == 0, "time-out above 1000 ms");
    CHECK(init_calls == 0 && spi_init_calls == 0 && clock_enabled == 0, "a refused configuration touches no pin and no clock");

    /* a peripheral that does not start leaves the driver down */
    spi_init_fail = 1;
    CHECK(NorSpi5_Init(&good) == 0, "the peripheral did not start");
    CHECK(ops->transfer(NULL, buf, NULL, 1u) == 0, "still down");
    spi_init_fail = 0;

    /* the start */
    CHECK(NorSpi5_Init(&good) == 1, "the start");
    CHECK(clock_enabled == 1, "the clock of SPI5 is on");
    CHECK(init_cs.Pin == (1u << 4) && init_cs.Mode == GPIO_MODE_OUTPUT_PP && init_cs.Pull == GPIO_PULLUP, "the chip select is a pushed-pull output with a pull-up");
    CHECK(init_bus.Pin == good.bus_pins && init_bus.Mode == GPIO_MODE_AF_PP && init_bus.Alternate == 5u && init_bus.Pull == GPIO_NOPULL, "the bus pins carry the alternate function of the configuration");
    CHECK(cs_level == 1, "the chip is not selected after the start");
    CHECK(spi_seen.Mode == SPI_MODE_MASTER && spi_seen.DataSize == SPI_DATASIZE_8BIT && spi_seen.CLKPolarity == SPI_POLARITY_LOW && spi_seen.CLKPhase == SPI_PHASE_1EDGE, "master, 8 bits, mode 0");
    CHECK(spi_seen.NSS == SPI_NSS_SOFT && spi_seen.BaudRatePrescaler == SPI_BAUDRATEPRESCALER_16 && spi_seen.FirstBit == SPI_FIRSTBIT_MSB, "software chip select, prescaler 16, MSB first");

    /* the commands of nor_flash.h end to end */
    CHECK(Nor_Probe(ops, NULL) == NOR_OK, "the identifier of the chip on the board");
    CHECK(timeout_seen == 20u, "the time-out of the configuration reaches the transfer");
    CHECK(Nor_EraseSector(ops, NULL, 4096u) == NOR_OK, "a sector is erased");
    for (unsigned i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)(i * 7u + 3u);
    CHECK(Nor_Write(ops, NULL, 4096u + 200u, buf, sizeof buf, 1, scratch) == NOR_OK, "a range across pages is written and read back");
    CHECK(Nor_Read(ops, NULL, 4096u + 200u, back, sizeof back) == NOR_OK && memcmp(buf, back, sizeof buf) == 0, "it reads back what was written");
    CHECK(tx_only_calls > 0 && rx_only_calls > 0, "the commands use the transmit and the receive directions");
    ops->select(NULL, 1);
    CHECK(ops->transfer(NULL, (const uint8_t *)"", scratch, 1u) == 1 && tx_rx_calls == 1, "a full-duplex transfer uses both buffers");
    ops->select(NULL, 0);
    CHECK(cs_level == 1, "the chip is left deselected");

    /* a transfer that fails is aborted and reported */
    transfer_fail = 1;
    aborts = 0;
    CHECK(Nor_ReadId(ops, NULL, &id) == NOR_ERR_TRANSFER, "a failed transfer is reported");
    CHECK(aborts >= 1, "a failed transfer is aborted");
    CHECK(ops->transfer(NULL, buf, buf, 1u) == 0 && ops->transfer(NULL, NULL, buf, 1u) == 0 && ops->transfer(NULL, buf, NULL, 1u) == 0, "each direction reports a failure");
    transfer_fail = 0;
    CHECK(ops->transfer(NULL, NULL, NULL, 1u) == 0, "a transfer with neither buffer is refused");
    CHECK(ops->transfer(NULL, buf, NULL, 0u) == 0 && ops->transfer(NULL, buf, NULL, 0x10000u) == 0, "an empty or too long transfer is refused");

    if (failures) {
        printf("nor_spi5: %d FAILED\n", failures);
        return 1;
    }
    printf("nor_spi5 host logic tests passed\n");
    return 0;
}
