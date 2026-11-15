/* =============================================================================
 * HYDRA-UMC Firmware - Host test stand-in for the STM32H7 HAL header (GPIO and SPI5)
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Just enough of the vendor header for the SPI5 driver of the NOR flash (src/mcu_stm32h757/CM7/NorSpi5.c) to compile with the host's plain gcc. The functions are declared here
 * and defined by the test, which plays the pins and the SPI peripheral.
 */
#ifndef HYDRA_UMC_HOST_STUB_H7_SPI5_HAL_H
#define HYDRA_UMC_HOST_STUB_H7_SPI5_HAL_H

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

extern SPI_TypeDef stub_spi5;
#define SPI5 (&stub_spi5)

#define GPIO_MODE_OUTPUT_PP 1u
#define GPIO_MODE_AF_PP 2u
#define GPIO_NOPULL 0u
#define GPIO_PULLUP 1u
#define GPIO_SPEED_FREQ_VERY_HIGH 3u

#define SPI_MODE_MASTER 1u
#define SPI_DIRECTION_2LINES 0u
#define SPI_DATASIZE_8BIT 7u
#define SPI_POLARITY_LOW 0u
#define SPI_PHASE_1EDGE 0u
#define SPI_NSS_SOFT 1u
#define SPI_BAUDRATEPRESCALER_16 3u
#define SPI_FIRSTBIT_MSB 0u
#define SPI_TIMODE_DISABLE 0u
#define SPI_CRCCALCULATION_DISABLE 0u

void stub_clock_enable(int which);
#define __HAL_RCC_SPI5_CLK_ENABLE() stub_clock_enable(1)

void HAL_GPIO_Init(GPIO_TypeDef *port, GPIO_InitTypeDef *init);
void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState state);
HAL_StatusTypeDef HAL_SPI_Init(SPI_HandleTypeDef *hspi);
HAL_StatusTypeDef HAL_SPI_TransmitReceive(SPI_HandleTypeDef *hspi, uint8_t *tx, uint8_t *rx, uint16_t size, uint32_t timeout);
HAL_StatusTypeDef HAL_SPI_Transmit(SPI_HandleTypeDef *hspi, uint8_t *tx, uint16_t size, uint32_t timeout);
HAL_StatusTypeDef HAL_SPI_Receive(SPI_HandleTypeDef *hspi, uint8_t *rx, uint16_t size, uint32_t timeout);
HAL_StatusTypeDef HAL_SPI_Abort(SPI_HandleTypeDef *hspi);

#endif /* HYDRA_UMC_HOST_STUB_H7_SPI5_HAL_H */
