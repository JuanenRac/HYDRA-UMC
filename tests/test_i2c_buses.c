/* =============================================================================
 * HYDRA-UMC Firmware - tests/test_i2c_buses.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Host test of I2C1, I2C2 and I2C3 on the CM7 (src/mcu_stm32h757/CM7/I2cBuses.c) against a double of the HAL (tests/stubs/h7i2cbuses): for each bus the clocks and the pins that
 * were started (PB6/PB7, PH4/PH5, PA8/PC9, alternate function 4, open drain, no pull), the instance and the timing word, the refusal of a bus that does not exist, of a timing of 0
 * and of a peripheral the HAL refuses, and the operations: a write and a read go to the shifted 7-bit address of the right instance, a bus that is not open, an address over 0x7F,
 * a missing buffer and a length of 0 or over 255 are refused before the HAL is called, a failing HAL is a failure, and a probe asks the device once. Evidence: simulator.
 *
 *   cc -std=c11 -Wall -Wextra -DHYDRA_I2C_BUSES_CM7=1 -Itests/stubs/h7i2cbuses -Isrc/mcu_stm32h757/CM7 -Isrc/mcu_stm32h757/Common \
 *      -o test_i2c_buses tests/test_i2c_buses.c src/mcu_stm32h757/CM7/I2cBuses.c
 */
#include <stdio.h>
#include <string.h>

#include "I2cBuses.h"

#if !HYDRA_I2C_BUSES_CM7
#error "this test is for the buses with their switch on: build it with -DHYDRA_I2C_BUSES_CM7=1"
#endif

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } } while (0)

GPIO_TypeDef stub_gpioa, stub_gpiob, stub_gpioc, stub_gpioh;
I2C_TypeDef stub_i2c1, stub_i2c2, stub_i2c3;

static int clocks[8];
static unsigned gpio_count;
static GPIO_TypeDef *gpio_ports[4];
static GPIO_InitTypeDef gpio_seen[4];
static unsigned init_count, tx_count, rx_count, ready_count;
static I2C_TypeDef *init_instance, *last_instance;
static I2C_InitTypeDef init_seen;
static int fail_init, fail_tx, fail_rx, fail_ready;
static uint16_t last_device, last_size;
static uint32_t last_timeout, last_trials;

void stub_clock_enable(int which) { if (which >= 0 && which < 8) clocks[which]++; }
void HAL_GPIO_Init(GPIO_TypeDef *port, GPIO_InitTypeDef *init)
{
    if (gpio_count < 4u) { gpio_ports[gpio_count] = port; gpio_seen[gpio_count] = *init; }
    gpio_count++;
}
HAL_StatusTypeDef HAL_I2C_Init(I2C_HandleTypeDef *h) { init_count++; init_instance = h->Instance; init_seen = h->Init; return fail_init ? HAL_ERROR : HAL_OK; }
HAL_StatusTypeDef HAL_I2C_Master_Transmit(I2C_HandleTypeDef *h, uint16_t d, uint8_t *p, uint16_t n, uint32_t t) { (void)p; tx_count++; last_instance = h->Instance; last_device = d; last_size = n; last_timeout = t; return fail_tx ? HAL_ERROR : HAL_OK; }
HAL_StatusTypeDef HAL_I2C_Master_Receive(I2C_HandleTypeDef *h, uint16_t d, uint8_t *p, uint16_t n, uint32_t t) { (void)p; rx_count++; last_instance = h->Instance; last_device = d; last_size = n; last_timeout = t; return fail_rx ? HAL_ERROR : HAL_OK; }
HAL_StatusTypeDef HAL_I2C_IsDeviceReady(I2C_HandleTypeDef *h, uint16_t d, uint32_t trials, uint32_t t) { ready_count++; last_instance = h->Instance; last_device = d; last_trials = trials; last_timeout = t; return fail_ready ? HAL_TIMEOUT : HAL_OK; }

static void reset(void)
{
    memset(clocks, 0, sizeof clocks);
    gpio_count = 0u;
    memset(gpio_seen, 0, sizeof gpio_seen);
    fail_init = fail_tx = fail_rx = fail_ready = 0;
}

int main(void)
{
    uint8_t buffer[255];
    memset(buffer, 0x3C, sizeof buffer);

    /* before anything is open every operation is refused and the HAL is not called */
    CHECK(I2cBuses_Write(I2CB_BUS1, 0x20u, buffer, 1u, 10u) == 1 && I2cBuses_Read(I2CB_BUS2, 0x20u, buffer, 1u, 10u) == 1 && I2cBuses_Probe(I2CB_BUS3, 0x20u, 10u) == 1, "closed buses refuse");
    CHECK(tx_count == 0u && rx_count == 0u && ready_count == 0u, "the HAL was not called");

    /* refusals at the start */
    reset();
    CHECK(I2cBuses_Open(3u, 0x40C15664UL) == 0 && I2cBuses_Open(I2CB_BUS1, 0u) == 0 && gpio_count == 0u && init_count == 0u, "no fourth bus, no timing of 0, nothing touched");
    fail_init = 1;
    CHECK(I2cBuses_Open(I2CB_BUS1, 0x40C15664UL) == 0, "a refused peripheral");
    CHECK(I2cBuses_Write(I2CB_BUS1, 0x20u, buffer, 1u, 10u) == 1, "and the bus stays closed");

    /* bus 1: PB6 and PB7 */
    reset();
    CHECK(I2cBuses_Open(I2CB_BUS1, 0x40C15664UL) == 1 && init_instance == I2C1 && init_seen.Timing == 0x40C15664UL, "I2C1 opens with the timing word");
    CHECK(clocks[2] == 1 && clocks[5] == 1 && gpio_count == 1u && gpio_ports[0] == GPIOB && gpio_seen[0].Pin == (GPIO_PIN_6 | GPIO_PIN_7), "port B, PB6 and PB7");
    CHECK(gpio_seen[0].Mode == GPIO_MODE_AF_OD && gpio_seen[0].Pull == GPIO_NOPULL && gpio_seen[0].Alternate == GPIO_AF4_I2C1, "open drain, no pull, alternate function 4");
    CHECK(init_seen.AddressingMode == I2C_ADDRESSINGMODE_7BIT && init_seen.OwnAddress1 == 0u && init_seen.DualAddressMode == I2C_DUALADDRESS_DISABLE && init_seen.GeneralCallMode == I2C_GENERALCALL_DISABLE && init_seen.NoStretchMode == I2C_NOSTRETCH_DISABLE, "7-bit, no own address");

    /* bus 2: PH4 and PH5 */
    reset();
    CHECK(I2cBuses_Open(I2CB_BUS2, 0x10E02246UL) == 1 && init_instance == I2C2 && gpio_count == 1u && gpio_ports[0] == GPIOH && gpio_seen[0].Pin == (GPIO_PIN_4 | GPIO_PIN_5) && clocks[4] == 1 && clocks[6] == 1, "I2C2 on port H");

    /* bus 3: PA8 and PC9 */
    reset();
    CHECK(I2cBuses_Open(I2CB_BUS3, 0x40C15664UL) == 1 && init_instance == I2C3 && gpio_count == 2u && gpio_ports[0] == GPIOA && gpio_seen[0].Pin == GPIO_PIN_8 && gpio_ports[1] == GPIOC && gpio_seen[1].Pin == GPIO_PIN_9, "I2C3 on PA8 and PC9");
    CHECK(clocks[1] == 1 && clocks[3] == 1 && clocks[7] == 1, "ports A and C and I2C3");

    /* operations */
    CHECK(I2cBuses_Write(I2CB_BUS1, 0x20u, buffer, 4u, 15u) == 0 && last_instance == I2C1 && last_device == 0x40u && last_size == 4u && last_timeout == 15u, "a write: shifted address, size, time-out");
    CHECK(I2cBuses_Read(I2CB_BUS2, 0x38u, buffer, 255u, 20u) == 0 && last_instance == I2C2 && last_device == 0x70u && last_size == 255u, "a read of 255 bytes on I2C2");
    CHECK(I2cBuses_Probe(I2CB_BUS3, 0x7Fu, 5u) == 0 && last_instance == I2C3 && last_device == 0xFEu && last_trials == 1u && last_timeout == 5u, "a probe asks once");

    /* refused before the HAL is called */
    const unsigned tx = tx_count, rx = rx_count, ready = ready_count;
    CHECK(I2cBuses_Write(3u, 0x20u, buffer, 1u, 1u) == 1 && I2cBuses_Write(I2CB_BUS1, 0x80u, buffer, 1u, 1u) == 1 && I2cBuses_Write(I2CB_BUS1, 0x20u, NULL, 1u, 1u) == 1, "no bus 4, no address over 0x7F, no buffer");
    CHECK(I2cBuses_Write(I2CB_BUS1, 0x20u, buffer, 0u, 1u) == 1 && I2cBuses_Write(I2CB_BUS1, 0x20u, buffer, 256u, 1u) == 1 && I2cBuses_Read(I2CB_BUS1, 0x20u, buffer, 0u, 1u) == 1, "no length of 0 or over 255");
    CHECK(I2cBuses_Probe(I2CB_BUS1, 0x80u, 1u) == 1 && I2cBuses_Probe(9u, 0x20u, 1u) == 1, "no probe of a bad address or bus");
    CHECK(tx_count == tx && rx_count == rx && ready_count == ready, "the HAL was not called for any of them");

    /* a failing HAL */
    fail_tx = 1; fail_rx = 1; fail_ready = 1;
    CHECK(I2cBuses_Write(I2CB_BUS1, 0x20u, buffer, 1u, 1u) == 1 && I2cBuses_Read(I2CB_BUS1, 0x20u, buffer, 1u, 1u) == 1 && I2cBuses_Probe(I2CB_BUS1, 0x20u, 1u) == 1, "a NACK is a failure");

    if (failures) {
        printf("i2c_buses host tests: %d FAILED\n", failures);
        return 1;
    }
    printf("i2c_buses host logic tests passed\n");
    return 0;
}
