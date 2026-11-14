/* =============================================================================
 * HYDRA-UMC Firmware - Host test stand-in for the STM32H7 HAL header (GPIO and SPI)
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Just enough of the vendor header for the SPI6 driver of the expansion cards (src/mcu_stm32h757/CM7/TmcCardSpi6.c) to compile with the host's plain gcc. The functions are
 * declared here and defined by the test, which plays the pins and the SPI peripheral and records what the driver did.
 */
#ifndef HYDRA_UMC_HOST_STUB_H7_SPI6_HAL_H
#define HYDRA_UMC_HOST_STUB_H7_SPI6_HAL_H

#include <stddef.h>
#include <stdint.h>

typedef enum { HAL_OK = 0, HAL_ERROR = 1, HAL_BUSY = 2, HAL_TIMEOUT = 3 } HAL_StatusTypeDef;
typedef enum { GPIO_PIN_RESET = 0, GPIO_PIN_SET = 1 } GPIO_PinState;

typedef struct { int port_id; } GPIO_TypeDef;
typedef struct { int instance_id; } SPI_TypeDef;

typedef struct {
    uint32_t Pin, Mode, Pull, Speed, Alternate;
} GPIO_InitTypeDef;

typedef struct {
    uint32_t Mode, Direction, DataSize, CLKPolarity, CLKPhase, NSS, BaudRatePrescaler, FirstBit, TIMode, CRCCalculation;
} SPI_InitTypeDef;

typedef struct {
    SPI_TypeDef *Instance;
    SPI_InitTypeDef Init;
} SPI_HandleTypeDef;

extern GPIO_TypeDef stub_gpioe, stub_gpiog;
extern SPI_TypeDef stub_spi6;
#define GPIOE (&stub_gpioe)
#define GPIOG (&stub_gpiog)
#define SPI6 (&stub_spi6)

#define GPIO_PIN_3 (1u << 3)
#define GPIO_PIN_5 (1u << 5)
#define GPIO_PIN_6 (1u << 6)
#define GPIO_PIN_12 (1u << 12)
#define GPIO_PIN_13 (1u << 13)
#define GPIO_PIN_14 (1u << 14)
#define GPIO_MODE_OUTPUT_PP 1u
#define GPIO_MODE_AF_PP 2u
#define GPIO_NOPULL 0u
#define GPIO_PULLUP 1u
#define GPIO_PULLDOWN 2u
#define GPIO_SPEED_FREQ_LOW 0u
#define GPIO_SPEED_FREQ_VERY_HIGH 3u
#define GPIO_AF5_SPI6 5u

#define SPI_MODE_MASTER 1u
#define SPI_DIRECTION_2LINES 0u
#define SPI_DATASIZE_8BIT 7u
#define SPI_POLARITY_LOW 0u
#define SPI_POLARITY_HIGH 1u
#define SPI_PHASE_1EDGE 0u
#define SPI_PHASE_2EDGE 1u
#define SPI_NSS_SOFT 1u
#define SPI_BAUDRATEPRESCALER_32 4u
#define SPI_FIRSTBIT_MSB 0u
#define SPI_TIMODE_DISABLE 0u
#define SPI_CRCCALCULATION_DISABLE 0u

void stub_clock_enable(int which);
#define __HAL_RCC_GPIOE_CLK_ENABLE() stub_clock_enable(1)
#define __HAL_RCC_GPIOG_CLK_ENABLE() stub_clock_enable(2)
#define __HAL_RCC_SPI6_CLK_ENABLE() stub_clock_enable(3)

void HAL_GPIO_Init(GPIO_TypeDef *port, GPIO_InitTypeDef *init);
void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState state);
HAL_StatusTypeDef HAL_SPI_Init(SPI_HandleTypeDef *hspi);
HAL_StatusTypeDef HAL_SPI_TransmitReceive(SPI_HandleTypeDef *hspi, uint8_t *tx, uint8_t *rx, uint16_t size, uint32_t timeout);
HAL_StatusTypeDef HAL_SPI_Abort(SPI_HandleTypeDef *hspi);

#endif /* HYDRA_UMC_HOST_STUB_H7_SPI6_HAL_H */
