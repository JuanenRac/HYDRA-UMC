/* =============================================================================
 * HYDRA-UMC Firmware - Host test stand-in for the STM32H7 HAL header (GPIO and I2C4)
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Just enough of the vendor header for the I2C4 bus of the F-RAM on the CM7 (src/mcu_stm32h757/CM7/FramI2c4.c) to compile with the host's plain gcc. The functions are declared
 * here and defined by the test, which plays the pins and the I2C peripheral and records what the driver did.
 */
#ifndef HYDRA_UMC_HOST_STUB_H7_I2C4_HAL_H
#define HYDRA_UMC_HOST_STUB_H7_I2C4_HAL_H

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

extern GPIO_TypeDef stub_gpiod;
extern I2C_TypeDef stub_i2c4;
#define GPIOD (&stub_gpiod)
#define I2C4 (&stub_i2c4)

#define GPIO_PIN_12 (1u << 12)
#define GPIO_PIN_13 (1u << 13)
#define GPIO_MODE_AF_OD 18u
#define GPIO_NOPULL 0u
#define GPIO_SPEED_FREQ_LOW 0u
#define GPIO_AF4_I2C4 4u

#define I2C_ADDRESSINGMODE_7BIT 1u
#define I2C_DUALADDRESS_DISABLE 0u
#define I2C_GENERALCALL_DISABLE 0u
#define I2C_NOSTRETCH_DISABLE 0u
#define I2C_MEMADD_SIZE_16BIT 2u

void stub_clock_enable(int which);
#define __HAL_RCC_GPIOD_CLK_ENABLE() stub_clock_enable(1)
#define __HAL_RCC_I2C4_CLK_ENABLE() stub_clock_enable(2)

void HAL_GPIO_Init(GPIO_TypeDef *port, GPIO_InitTypeDef *init);
HAL_StatusTypeDef HAL_I2C_Init(I2C_HandleTypeDef *hi2c);
HAL_StatusTypeDef HAL_I2C_Mem_Read(I2C_HandleTypeDef *hi2c, uint16_t device, uint16_t memory_address, uint16_t memory_size, uint8_t *data, uint16_t size, uint32_t timeout);
HAL_StatusTypeDef HAL_I2C_Mem_Write(I2C_HandleTypeDef *hi2c, uint16_t device, uint16_t memory_address, uint16_t memory_size, uint8_t *data, uint16_t size, uint32_t timeout);

#endif /* HYDRA_UMC_HOST_STUB_H7_I2C4_HAL_H */
