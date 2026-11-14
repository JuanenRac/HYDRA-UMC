/* =============================================================================
 * HYDRA-UMC Firmware - tests/test_tmc_card_spi6.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Host test of the SPI6 driver of the TMC5160 cards (src/mcu_stm32h757/CM7/TmcCardSpi6.c) against a double of the HAL (tests/stubs/h7spi6): the double plays the pins and the
 * peripheral and records every call. It checks the configuration the driver gives the peripheral (master, 8 bits, mode 3, prescaler 32, software chip select, alternate function 5),
 * the pins and their idle levels, the order of the pins during a write and a read (the select lines change only while the strobe is high), the refusal of a bad configuration or of
 * a call before the start, the abort of a datagram that failed and the release of the card after it. Evidence: simulator.
 *
 *   cc -std=c11 -Wall -Wextra -DHYDRA_PLATFORM_H757 -DHYDRA_TMC_CARD_SPI6=1 -Itests/stubs/h7spi6 -Isrc/mcu_stm32h757/CM7 -Isrc/mcu_stm32h757/Common \
 *      -o test_tmc_card_spi6 tests/test_tmc_card_spi6.c src/mcu_stm32h757/CM7/TmcCardSpi6.c
 */
#include <stdio.h>
#include <string.h>

#include "stm32h7xx_hal.h"
#include "TmcCardSpi6.h"

#if !HYDRA_TMC_CARD_SPI6
#error "this test is for the driver with its switch on: build it with -DHYDRA_TMC_CARD_SPI6=1"
#endif

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } } while (0)

GPIO_TypeDef stub_gpioe = {1}, stub_gpiog = {2};
SPI_TypeDef stub_spi6 = {6};

/* what the double saw */
static int clock_mask;
static GPIO_InitTypeDef init_e, init_g_cs, init_g_bus;
static int init_count;
static int pin_e5, pin_e6, pin_g3 = -1;       /* levels now (-1: never driven) */
static int select_changed_with_cs_low;
static int spi_init_calls, spi_init_fail;
static SPI_InitTypeDef spi_seen;
static int transfers, transfer_fail, aborts;
static uint8_t last_tx[5];
static int select_during_transfer, cs_during_transfer;
static uint32_t timeout_seen;
static uint8_t answers[2][5];
static int delays;
static uint32_t delay_seen;

void stub_clock_enable(int which) { clock_mask |= 1 << which; }

void HAL_GPIO_Init(GPIO_TypeDef *port, GPIO_InitTypeDef *init)
{
    init_count++;
    if (port == GPIOE) init_e = *init;
    else if (init->Mode == GPIO_MODE_AF_PP) init_g_bus = *init;
    else init_g_cs = *init;
}

void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState state)
{
    if (port == GPIOE) {
        if (pin_g3 == 0) select_changed_with_cs_low = 1;
        if (pin == GPIO_PIN_5) pin_e5 = (int)state;
        if (pin == GPIO_PIN_6) pin_e6 = (int)state;
    } else if (port == GPIOG && pin == GPIO_PIN_3) {
        pin_g3 = (int)state;
    }
}

HAL_StatusTypeDef HAL_SPI_Init(SPI_HandleTypeDef *hspi)
{
    spi_init_calls++;
    spi_seen = hspi->Init;
    return spi_init_fail ? HAL_ERROR : HAL_OK;
}

HAL_StatusTypeDef HAL_SPI_TransmitReceive(SPI_HandleTypeDef *hspi, uint8_t *tx, uint8_t *rx, uint16_t size, uint32_t timeout)
{
    (void)hspi;
    CHECK(size == 5u, "a datagram is five bytes");
    memcpy(last_tx, tx, 5);
    select_during_transfer = pin_e5 | (pin_e6 << 1);
    cs_during_transfer = pin_g3;
    timeout_seen = timeout;
    if (transfer_fail) return HAL_TIMEOUT;
    memcpy(rx, answers[transfers < 2 ? transfers : 1], 5);
    transfers++;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_SPI_Abort(SPI_HandleTypeDef *hspi)
{
    (void)hspi;
    aborts++;
    return HAL_OK;
}

static void delay_us(uint32_t microseconds)
{
    delays++;
    delay_seen = microseconds;
}

static void forget(void)
{
    transfers = 0;
    transfer_fail = 0;
    aborts = 0;
    delays = 0;
    select_changed_with_cs_low = 0;
}

int main(void)
{
    TmcReply reply;
    const TmcCardSpi6Config good = {5u, 20u, delay_us};

    /* before a successful start nothing moves */
    CHECK(TmcCardSpi6_Write(1u, 0x07u, 0x00u, 4UL, &reply) == TMC_ERR_TRANSFER, "a write before the start is refused");
    CHECK(TmcCardSpi6_Read(1u, 0x07u, 0x04u, &reply) == TMC_ERR_TRANSFER, "a read before the start is refused");
    CHECK(init_count == 0 && transfers == 0 && pin_g3 == -1, "nothing was touched before the start");

    /* a bad configuration is refused and touches nothing */
    {
        TmcCardSpi6Config bad = good;
        CHECK(TmcCardSpi6_Init(NULL) == 0, "no configuration");
        bad.delay_us = NULL;
        CHECK(TmcCardSpi6_Init(&bad) == 0, "no delay function");
        bad = good;
        bad.settle_us = 0u;
        CHECK(TmcCardSpi6_Init(&bad) == 0, "settle time 0");
        bad.settle_us = 1001u;
        CHECK(TmcCardSpi6_Init(&bad) == 0, "settle time above 1000 us");
        bad = good;
        bad.transfer_timeout_ms = 0u;
        CHECK(TmcCardSpi6_Init(&bad) == 0, "time-out 0");
        bad.transfer_timeout_ms = 1001u;
        CHECK(TmcCardSpi6_Init(&bad) == 0, "time-out above 1000 ms");
        CHECK(init_count == 0 && spi_init_calls == 0 && clock_mask == 0, "a refused configuration touches no pin and no clock");
    }

    /* a peripheral that does not start leaves the driver down */
    spi_init_fail = 1;
    CHECK(TmcCardSpi6_Init(&good) == 0, "the peripheral refused to start");
    CHECK(TmcCardSpi6_Write(1u, 0x07u, 0x00u, 4UL, &reply) == TMC_ERR_TRANSFER, "a driver that did not start answers with a failure");
    spi_init_fail = 0;

    /* the start */
    CHECK(TmcCardSpi6_Init(&good) == 1, "the start succeeds");
    CHECK(clock_mask == ((1 << 1) | (1 << 2) | (1 << 3)), "the clocks of port E, port G and SPI6 are on");
    CHECK(init_e.Pin == (GPIO_PIN_5 | GPIO_PIN_6) && init_e.Mode == GPIO_MODE_OUTPUT_PP && init_e.Pull == GPIO_PULLDOWN, "EXP_GPIO1 and EXP_GPIO2 are outputs pulled down");
    CHECK(init_g_cs.Pin == GPIO_PIN_3 && init_g_cs.Mode == GPIO_MODE_OUTPUT_PP && init_g_cs.Pull == GPIO_PULLUP, "PG3 is the strobe output, pulled up");
    CHECK(init_g_bus.Pin == (GPIO_PIN_12 | GPIO_PIN_13 | GPIO_PIN_14) && init_g_bus.Alternate == GPIO_AF5_SPI6, "PG12, PG13 and PG14 are SPI6 on alternate function 5");
    CHECK(pin_g3 == 1 && pin_e5 == 0 && pin_e6 == 0, "idle: strobe high and no card selected");
    CHECK(spi_seen.Mode == SPI_MODE_MASTER && spi_seen.DataSize == SPI_DATASIZE_8BIT && spi_seen.Direction == SPI_DIRECTION_2LINES, "master, 8 bits, two lines");
    CHECK(spi_seen.CLKPolarity == SPI_POLARITY_HIGH && spi_seen.CLKPhase == SPI_PHASE_2EDGE, "SPI mode 3, as the TMC5160 wants");
    CHECK(spi_seen.NSS == SPI_NSS_SOFT && spi_seen.BaudRatePrescaler == SPI_BAUDRATEPRESCALER_32 && spi_seen.FirstBit == SPI_FIRSTBIT_MSB, "software chip select, prescaler 32, most significant bit first");
    CHECK(spi_seen.TIMode == SPI_TIMODE_DISABLE && spi_seen.CRCCalculation == SPI_CRCCALCULATION_DISABLE, "no TI mode and no hardware CRC");

    /* a write: select lines change while the strobe is high, the strobe is low during the datagram, then the card is let go */
    forget();
    answers[0][0] = 0x01;
    CHECK(TmcCardSpi6_Write(2u, 0x07u, 0x10u, 0x00071F0AUL, &reply) == TMC_OK, "the write succeeds");
    CHECK(last_tx[0] == 0x90 && last_tx[1] == 0x00 && last_tx[2] == 0x07 && last_tx[3] == 0x1F && last_tx[4] == 0x0A, "the golden IHOLD_IRUN datagram goes out");
    CHECK(select_during_transfer == 2 && cs_during_transfer == 0, "slot 2 is selected and the strobe is low during the datagram");
    CHECK(!select_changed_with_cs_low, "the select lines never changed while the strobe was low");
    CHECK(pin_g3 == 1 && pin_e5 == 0 && pin_e6 == 0, "the strobe is high and no card is selected afterwards");
    CHECK(delays == 1 && delay_seen == 5u && timeout_seen == 20u && reply.status == 0x01, "settled once for the configured time, the configured time-out, status returned");

    /* a read is two datagrams and the data of the second is the answer */
    forget();
    answers[0][0] = 0x08;
    answers[1][0] = 0x08;
    answers[1][1] = 0xCA;
    answers[1][4] = 0xFE;
    CHECK(TmcCardSpi6_Read(3u, 0x04u, 0x04u, &reply) == TMC_OK, "the read succeeds");
    CHECK(transfers == 2 && reply.data == 0xCA0000FEUL && last_tx[0] == 0x04, "two datagrams, the data of the second, no write bit");
    CHECK(select_during_transfer == 3 && !select_changed_with_cs_low, "slot 3 selected through both lines, never changed under the strobe");

    /* refusals move nothing */
    forget();
    CHECK(TmcCardSpi6_Write(0u, 0x07u, 0, 0, &reply) == TMC_ERR_SLOT, "slot 0 refused");
    CHECK(TmcCardSpi6_Write(1u, 0x06u, 0, 0, &reply) == TMC_ERR_NOT_READY, "a slot that is not READY refused");
    CHECK(TmcCardSpi6_Read(2u, 0x00u, 0, &reply) == TMC_ERR_NOT_READY, "a read of a slot that is not READY refused");
    CHECK(transfers == 0 && delays == 0 && pin_g3 == 1 && pin_e5 == 0 && pin_e6 == 0, "nothing moved on the lines");

    /* a datagram that fails is aborted and the card is released; a read stops after the first one */
    forget();
    transfer_fail = 1;
    CHECK(TmcCardSpi6_Write(1u, 0x01u, 0, 0, &reply) == TMC_ERR_TRANSFER, "the failure is reported");
    CHECK(aborts == 1 && pin_g3 == 1 && pin_e5 == 0 && pin_e6 == 0, "the transfer was aborted and the card is released");
    CHECK(TmcCardSpi6_Read(1u, 0x01u, 0, &reply) == TMC_ERR_TRANSFER && aborts == 2, "a read stops after the first failing datagram");

    if (failures) {
        printf("test_tmc_card_spi6: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_tmc_card_spi6: all checks passed\n");
    return 0;
}
