/* =============================================================================
 * HYDRA-UMC Firmware - Host test stand-in for the STM32H7 HAL header (GPIO and I2C1 to I2C3)
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Just enough of the vendor header for the three general I2C buses of the CM7 (src/mcu_stm32h757/CM7/I2cBuses.c) to compile with the host's plain gcc. The functions are declared
 * here and defined by the test, which plays the pins, the clocks and the peripherals and records what the driver did.
 */
#ifndef HYDRA_UMC_HOST_STUB_H7_I2CBUSES_HAL_H
#define HYDRA_UMC_HOST_STUB_H7_I2CBUSES_HAL_H

#include <stddef.h>
#include <stdint.h>

typedef enum { HAL_OK = 0, HAL_ERROR = 1, HAL_BUSY = 2, HAL_TIMEOUT = 3 } HAL_StatusTypeDef;

typedef struct { int port_id; } GPIO_TypeDef;
typedef struct { int instance_id; } I2C_TypeDef;

typedef struct {
    uint32_t Pin, Mode, Pull, Speed, Alternate;
} GPIO_InitTypeDef;

typedef struct {
    uint32_t Timing, OwnAddress1, AddressingMode, DualAddressMode, OwnAddress2, GeneralCallMode, NoStretchMode;
} I2C_InitTypeDef;

typedef struct {
    I2C_TypeDef *Instance;
    I2C_InitTypeDef Init;
} I2C_HandleTypeDef;

extern GPIO_TypeDef stub_gpioa, stub_gpiob, stub_gpioc, stub_gpioh;
extern I2C_TypeDef stub_i2c1, stub_i2c2, stub_i2c3;
#define GPIOA (&stub_gpioa)
#define GPIOB (&stub_gpiob)
#define GPIOC (&stub_gpioc)
#define GPIOH (&stub_gpioh)
#define I2C1 (&stub_i2c1)
#define I2C2 (&stub_i2c2)
#define I2C3 (&stub_i2c3)

#define GPIO_PIN_4 (1u << 4)
#define GPIO_PIN_5 (1u << 5)
#define GPIO_PIN_6 (1u << 6)
#define GPIO_PIN_7 (1u << 7)
#define GPIO_PIN_8 (1u << 8)
#define GPIO_PIN_9 (1u << 9)
#define GPIO_MODE_AF_OD 18u
#define GPIO_NOPULL 0u
#define GPIO_SPEED_FREQ_LOW 0u
#define GPIO_AF4_I2C1 4u

#define I2C_ADDRESSINGMODE_7BIT 1u
#define I2C_DUALADDRESS_DISABLE 0u
#define I2C_GENERALCALL_DISABLE 0u
#define I2C_NOSTRETCH_DISABLE 0u

void stub_clock_enable(int which);
#define __HAL_RCC_GPIOA_CLK_ENABLE() stub_clock_enable(1)
#define __HAL_RCC_GPIOB_CLK_ENABLE() stub_clock_enable(2)
#define __HAL_RCC_GPIOC_CLK_ENABLE() stub_clock_enable(3)
#define __HAL_RCC_GPIOH_CLK_ENABLE() stub_clock_enable(4)
#define __HAL_RCC_I2C1_CLK_ENABLE() stub_clock_enable(5)
#define __HAL_RCC_I2C2_CLK_ENABLE() stub_clock_enable(6)
#define __HAL_RCC_I2C3_CLK_ENABLE() stub_clock_enable(7)

void HAL_GPIO_Init(GPIO_TypeDef *port, GPIO_InitTypeDef *init);
HAL_StatusTypeDef HAL_I2C_Init(I2C_HandleTypeDef *hi2c);
HAL_StatusTypeDef HAL_I2C_Master_Transmit(I2C_HandleTypeDef *hi2c, uint16_t device, uint8_t *data, uint16_t size, uint32_t timeout);
HAL_StatusTypeDef HAL_I2C_Master_Receive(I2C_HandleTypeDef *hi2c, uint16_t device, uint8_t *data, uint16_t size, uint32_t timeout);
HAL_StatusTypeDef HAL_I2C_IsDeviceReady(I2C_HandleTypeDef *hi2c, uint16_t device, uint32_t trials, uint32_t timeout);

#endif /* HYDRA_UMC_HOST_STUB_H7_I2CBUSES_HAL_H */
