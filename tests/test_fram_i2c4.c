/* =============================================================================
 * HYDRA-UMC Firmware - tests/test_fram_i2c4.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Host test of I2C4 on the CM7 as the bus of the F-RAM (src/mcu_stm32h757/CM7/FramI2c4.c) against a double of the HAL (tests/stubs/h7i2c4) in which the double plays the pins, the
 * clocks, the peripheral and an 8 KiB chip: what the start-up wrote (pins PD12 and PD13, alternate function 4, open drain, no pull, the timing word, 7-bit addressing), the refusal of
 * a timing of 0, of a missing bus and of a peripheral that the HAL refuses, and the whole driver of CM4/fram_i2c.h running over the bus with the chip double: a write that is read
 * back, a read, a transfer that is too short, a NACK that is tried again, and the yield between chunks. Evidence: simulator.
 *
 *   cc -std=c11 -Wall -Wextra -DHYDRA_FRAM_I2C4_CM7=1 -Itests/stubs/h7i2c4 -Isrc/mcu_stm32h757/CM7 -Isrc/mcu_stm32h757/CM4 -Isrc/mcu_stm32h757/Common \
 *      -o test_fram_i2c4 tests/test_fram_i2c4.c src/mcu_stm32h757/CM7/FramI2c4.c
 */
#include <stdio.h>
#include <string.h>

#include "FramI2c4.h"

#if !HYDRA_FRAM_I2C4_CM7
#error "this test is for the bus with its switch on: build it with -DHYDRA_FRAM_I2C4_CM7=1"
#endif

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } } while (0)

GPIO_TypeDef stub_gpiod;
I2C_TypeDef stub_i2c4;

static uint8_t chip[8192];
static unsigned clock_calls, gpio_calls, init_calls, read_calls, write_calls;
static int clock_seen[4];
static GPIO_TypeDef *gpio_port;
static GPIO_InitTypeDef gpio_seen;
static I2C_InitTypeDef init_seen;
static I2C_TypeDef *instance_seen;
static int fail_init, nack_reads, nack_writes, corrupt_next_read;
static uint16_t device_seen, memsize_seen;
static unsigned yields;

void stub_clock_enable(int which) { clock_calls++; if (which >= 0 && which < 4) clock_seen[which]++; }
void HAL_GPIO_Init(GPIO_TypeDef *port, GPIO_InitTypeDef *init) { gpio_calls++; gpio_port = port; gpio_seen = *init; }

HAL_StatusTypeDef HAL_I2C_Init(I2C_HandleTypeDef *h)
{
    init_calls++;
    init_seen = h->Init;
    instance_seen = h->Instance;
    return fail_init ? HAL_ERROR : HAL_OK;
}

HAL_StatusTypeDef HAL_I2C_Mem_Read(I2C_HandleTypeDef *h, uint16_t device, uint16_t address, uint16_t size_code, uint8_t *data, uint16_t size, uint32_t timeout)
{
    (void)h; (void)timeout;
    read_calls++;
    device_seen = device;
    memsize_seen = size_code;
    if (nack_reads > 0) { nack_reads--; return HAL_ERROR; }
    memcpy(data, &chip[address], size);
    if (corrupt_next_read) { corrupt_next_read = 0; data[0] ^= 0xFFu; }
    return HAL_OK;
}

HAL_StatusTypeDef HAL_I2C_Mem_Write(I2C_HandleTypeDef *h, uint16_t device, uint16_t address, uint16_t size_code, uint8_t *data, uint16_t size, uint32_t timeout)
{
    (void)h; (void)timeout;
    write_calls++;
    device_seen = device;
    memsize_seen = size_code;
    if (nack_writes > 0) { nack_writes--; return HAL_ERROR; }
    memcpy(&chip[address], data, size);
    return HAL_OK;
}

static void yield(void *context) { (void)context; yields++; }

int main(void)
{
    FramI2cBus_t bus;
    int marker = 0;

    /* refusals before the pins are touched */
    CHECK(FramI2c4_Open(0u, NULL, NULL, &bus) == 0 && gpio_calls == 0u && init_calls == 0u, "a timing of 0 is refused and nothing is touched");
    CHECK(FramI2c4_Open(0x40C15664UL, NULL, NULL, NULL) == 0 && gpio_calls == 0u, "no bus is refused and nothing is touched");

    /* the good start */
    memset(&bus, 0, sizeof bus);
    CHECK(FramI2c4_Open(0x40C15664UL, yield, &marker, &bus) == 1, "a good start");
    CHECK(clock_seen[1] == 1 && clock_seen[2] == 1 && clock_calls == 2u, "the clocks of port D and of I2C4");
    CHECK(gpio_port == GPIOD && gpio_seen.Pin == (GPIO_PIN_12 | GPIO_PIN_13) && gpio_seen.Mode == GPIO_MODE_AF_OD && gpio_seen.Pull == GPIO_NOPULL && gpio_seen.Alternate == GPIO_AF4_I2C4, "SCL and SDA: open drain, no pull, alternate function 4");
    CHECK(instance_seen == I2C4 && init_seen.Timing == 0x40C15664UL && init_seen.AddressingMode == I2C_ADDRESSINGMODE_7BIT && init_seen.OwnAddress1 == 0u, "the timing word and 7-bit addressing");
    CHECK(init_seen.DualAddressMode == I2C_DUALADDRESS_DISABLE && init_seen.GeneralCallMode == I2C_GENERALCALL_DISABLE && init_seen.NoStretchMode == I2C_NOSTRETCH_DISABLE, "no second address, no general call, stretching on");
    CHECK(bus.transfer == FramI2c4_Transfer && bus.yield == yield && bus.context == &marker, "the bus for the driver");

    /* a peripheral that the HAL refuses leaves the bus untouched */
    FramI2cBus_t refused;
    memset(&refused, 0, sizeof refused);
    fail_init = 1;
    CHECK(FramI2c4_Open(0x40C15664UL, yield, NULL, &refused) == 0 && refused.transfer == NULL, "a refused peripheral gives no bus");
    fail_init = 0;

    /* the driver of the chip over this bus */
    FramI2c_t fram;
    FramI2c_Init(&fram, &bus);
    uint8_t out[100], back[100];
    for (unsigned i = 0; i < sizeof out; i++) out[i] = (uint8_t)(i * 7u + 1u);
    CHECK(FramI2c_Write(&fram, 0x123u, out, sizeof out) == 0 && memcmp(&chip[0x123], out, sizeof out) == 0, "a write of 100 bytes reaches the chip, read back chunk by chunk");
    CHECK(device_seen == (uint16_t)(FRAM_I2C_ADDRESS << 1) && memsize_seen == I2C_MEMADD_SIZE_16BIT, "7-bit address 0x50 shifted, 16-bit memory address");
    CHECK(yields >= 3u, "the yield runs between the chunks");
    CHECK(FramI2c_Read(&fram, 0x123u, back, sizeof back) == 0 && memcmp(back, out, sizeof out) == 0, "a read gives the same bytes");

    /* a NACK is tried again, a chunk that reads back wrong is tried again, a chip that never answers fails the access */
    memset(chip, 0, sizeof chip);
    nack_writes = 1;
    CHECK(FramI2c_Write(&fram, 0u, out, 10u) == 0 && fram.retries_used >= 1u && memcmp(chip, out, 10u) == 0, "one NACK on a write is tried again");
    corrupt_next_read = 1;
    CHECK(FramI2c_Write(&fram, 0x40u, out, 10u) == 0 && memcmp(&chip[0x40], out, 10u) == 0, "a chunk that reads back wrong is written again");
    nack_reads = 99;
    CHECK(FramI2c_Read(&fram, 0u, back, 10u) == 1, "a chip that never answers fails the read");
    nack_reads = 0;
    nack_writes = 99;
    CHECK(FramI2c_Write(&fram, 0u, out, 10u) == 1, "a chip that never answers fails the write");
    nack_writes = 0;

    /* the transfer itself: too short, a read, a write of the bytes that follow the address */
    unsigned reads = read_calls, writes = write_calls;
    const uint8_t only_address[1] = {0x01u};
    CHECK(FramI2c4_Transfer(NULL, FRAM_I2C_ADDRESS, only_address, 1u, NULL, 0u, 20u) == 1 && read_calls == reads && write_calls == writes, "a transfer without the two address bytes is refused");
    const uint8_t request[4] = {0x00u, 0x10u, 0xAAu, 0xBBu};
    CHECK(FramI2c4_Transfer(NULL, FRAM_I2C_ADDRESS, request, 4u, NULL, 0u, 20u) == 0 && chip[0x10] == 0xAAu && chip[0x11] == 0xBBu, "a write puts the bytes after the address at the address");
    uint8_t two[2] = {0, 0};
    const uint8_t address_only[2] = {0x00u, 0x10u};
    CHECK(FramI2c4_Transfer(NULL, FRAM_I2C_ADDRESS, address_only, 2u, two, 2u, 20u) == 0 && two[0] == 0xAAu && two[1] == 0xBBu, "a read repeats the start and reads");

    if (failures) {
        printf("fram_i2c4 host tests: %d FAILED\n", failures);
        return 1;
    }
    printf("fram_i2c4 host logic tests passed\n");
    return 0;
}
