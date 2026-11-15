/* =============================================================================
 * HYDRA-UMC - src/mcu_stm32h757/CM7/I2cBuses.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * I2C1, I2C2 and I2C3 of the CM7; see I2cBuses.h and Common/i2c_plan.h. The pins are those of docs/platform_profiles.json (new_peripherals).
 * ========================================================================== */
#include "I2cBuses.h"

#if HYDRA_I2C_BUSES_CM7

static I2C_HandleTypeDef g_bus[I2CB_COUNT];
static int g_open[I2CB_COUNT];

static void I2cBuses_Pins(GPIO_TypeDef *port, uint32_t pins)
{
    GPIO_InitTypeDef init = {0};
    init.Pin = pins;
    init.Mode = GPIO_MODE_AF_OD;
    init.Pull = GPIO_NOPULL;
    init.Speed = GPIO_SPEED_FREQ_LOW;
    init.Alternate = GPIO_AF4_I2C1; /* alternate function 4 on all three buses */
    HAL_GPIO_Init(port, &init);
}

static void I2cBuses_StartClocks(unsigned bus)
{
    if (bus == I2CB_BUS1) {
        __HAL_RCC_GPIOB_CLK_ENABLE();
        __HAL_RCC_I2C1_CLK_ENABLE();
        I2cBuses_Pins(GPIOB, GPIO_PIN_6 | GPIO_PIN_7);
    } else if (bus == I2CB_BUS2) {
        __HAL_RCC_GPIOH_CLK_ENABLE();
        __HAL_RCC_I2C2_CLK_ENABLE();
        I2cBuses_Pins(GPIOH, GPIO_PIN_4 | GPIO_PIN_5);
    } else {
        __HAL_RCC_GPIOA_CLK_ENABLE();
        __HAL_RCC_GPIOC_CLK_ENABLE();
        __HAL_RCC_I2C3_CLK_ENABLE();
        I2cBuses_Pins(GPIOA, GPIO_PIN_8);
        I2cBuses_Pins(GPIOC, GPIO_PIN_9);
    }
}

int I2cBuses_Open(unsigned bus, uint32_t timingr)
{
    if (bus >= I2CB_COUNT || timingr == 0u) return 0;
    I2cBuses_StartClocks(bus);
    I2C_HandleTypeDef *h = &g_bus[bus];
    h->Instance = bus == I2CB_BUS1 ? I2C1 : (bus == I2CB_BUS2 ? I2C2 : I2C3);
    h->Init.Timing = timingr;
    h->Init.OwnAddress1 = 0u;
    h->Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
    h->Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    h->Init.OwnAddress2 = 0u;
    h->Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    h->Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
    g_open[bus] = HAL_I2C_Init(h) == HAL_OK;
    return g_open[bus];
}

static int I2cBuses_Usable(unsigned bus, uint8_t address7, const void *data, uint16_t length)
{
    return bus < I2CB_COUNT && g_open[bus] && address7 <= 0x7Fu && data != NULL && length >= 1u && length <= 255u;
}

int I2cBuses_Write(unsigned bus, uint8_t address7, const uint8_t *data, uint16_t length, uint32_t timeout_ms)
{
    if (!I2cBuses_Usable(bus, address7, data, length)) return 1;
    return HAL_I2C_Master_Transmit(&g_bus[bus], (uint16_t)(address7 << 1), (uint8_t *)(uintptr_t)data, length, timeout_ms) == HAL_OK ? 0 : 1;
}

int I2cBuses_Read(unsigned bus, uint8_t address7, uint8_t *data, uint16_t length, uint32_t timeout_ms)
{
    if (!I2cBuses_Usable(bus, address7, data, length)) return 1;
    return HAL_I2C_Master_Receive(&g_bus[bus], (uint16_t)(address7 << 1), data, length, timeout_ms) == HAL_OK ? 0 : 1;
}

int I2cBuses_Probe(unsigned bus, uint8_t address7, uint32_t timeout_ms)
{
    uint8_t unused = 0u;
    if (!I2cBuses_Usable(bus, address7, &unused, 1u)) return 1;
    return HAL_I2C_IsDeviceReady(&g_bus[bus], (uint16_t)(address7 << 1), 1u, timeout_ms) == HAL_OK ? 0 : 1;
}

#endif /* HYDRA_I2C_BUSES_CM7 */
