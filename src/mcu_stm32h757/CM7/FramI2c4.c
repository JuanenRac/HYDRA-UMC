/* =============================================================================
 * HYDRA-UMC - src/mcu_stm32h757/CM7/FramI2c4.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * I2C4 of the CM7 as the bus of the F-RAM; see FramI2c4.h and Common/i2c_plan.h. The access is the one of CM4/SpiApp.c: the two address bytes of the driver are the memory address of
 * the HAL, a read repeats the start and a write sends the data that follows the address.
 * ========================================================================== */
#include "FramI2c4.h"

#if HYDRA_FRAM_I2C4_CM7

static I2C_HandleTypeDef hi2c4_fram;

int FramI2c4_Transfer(void *context, uint8_t address7, const uint8_t *tx, uint32_t tx_length, uint8_t *rx, uint32_t rx_length, uint32_t timeout_ms)
{
    (void)context;
    if (tx_length < 2u) return 1;
    const uint16_t memory_address = (uint16_t)(((uint16_t)tx[0] << 8) | tx[1]);
    const uint16_t device = (uint16_t)(address7 << 1);
    if (rx_length > 0u) {
        return HAL_I2C_Mem_Read(&hi2c4_fram, device, memory_address, I2C_MEMADD_SIZE_16BIT, rx, (uint16_t)rx_length, timeout_ms) == HAL_OK ? 0 : 1;
    }
    return HAL_I2C_Mem_Write(&hi2c4_fram, device, memory_address, I2C_MEMADD_SIZE_16BIT, (uint8_t *)(uintptr_t)&tx[2], (uint16_t)(tx_length - 2u), timeout_ms) == HAL_OK ? 0 : 1;
}

int FramI2c4_Open(uint32_t timingr, void (*yield)(void *context), void *context, FramI2cBus_t *bus)
{
    if (timingr == 0u || bus == NULL) return 0;
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_I2C4_CLK_ENABLE();
    GPIO_InitTypeDef pins = {0};
    pins.Pin = GPIO_PIN_12 | GPIO_PIN_13;
    pins.Mode = GPIO_MODE_AF_OD;
    pins.Pull = GPIO_NOPULL;
    pins.Speed = GPIO_SPEED_FREQ_LOW;
    pins.Alternate = GPIO_AF4_I2C4;
    HAL_GPIO_Init(GPIOD, &pins);
    hi2c4_fram.Instance = I2C4;
    hi2c4_fram.Init.Timing = timingr;
    hi2c4_fram.Init.OwnAddress1 = 0u;
    hi2c4_fram.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
    hi2c4_fram.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    hi2c4_fram.Init.OwnAddress2 = 0u;
    hi2c4_fram.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    hi2c4_fram.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
    if (HAL_I2C_Init(&hi2c4_fram) != HAL_OK) return 0;
    bus->transfer = FramI2c4_Transfer;
    bus->yield = yield;
    bus->context = context;
    return 1;
}

#endif /* HYDRA_FRAM_I2C4_CM7 */
