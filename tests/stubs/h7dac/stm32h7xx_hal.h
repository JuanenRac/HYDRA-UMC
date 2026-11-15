/* =============================================================================
 * HYDRA-UMC Firmware - Host test stand-in for the STM32H7 HAL header (DAC)
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Just enough of the vendor header for the analog outputs of the CM7 (src/mcu_stm32h757/CM7/DacOut.c) to compile with the host's plain gcc. The names and the fields are the ones of
 * the HAL of the H7; the functions are declared here and defined by the test, which plays the converter and records what the driver did.
 */
#ifndef HYDRA_UMC_HOST_STUB_H7_DAC_HAL_H
#define HYDRA_UMC_HOST_STUB_H7_DAC_HAL_H

#include <stddef.h>
#include <stdint.h>

typedef enum { HAL_OK = 0, HAL_ERROR = 1, HAL_BUSY = 2, HAL_TIMEOUT = 3 } HAL_StatusTypeDef;

typedef struct { int instance_id; } DAC_TypeDef;
typedef struct { DAC_TypeDef *Instance; } DAC_HandleTypeDef;
typedef struct { uint32_t DAC_SampleAndHold, DAC_Trigger, DAC_OutputBuffer, DAC_ConnectOnChipPeripheral, DAC_UserTrimming; } DAC_ChannelConfTypeDef;

extern DAC_TypeDef stub_dac1;
#define DAC1 (&stub_dac1)

#define DAC_CHANNEL_1 0x00000000U
#define DAC_CHANNEL_2 0x00000010U
#define DAC_SAMPLEANDHOLD_DISABLE 0U
#define DAC_TRIGGER_NONE 0U
#define DAC_OUTPUTBUFFER_ENABLE 0U
#define DAC_CHIPCONNECT_EXTERNAL 1U
#define DAC_TRIMMING_FACTORY 0U
#define DAC_ALIGN_12B_R 0U

void stub_clock_enable(int which);
#define __HAL_RCC_DAC12_CLK_ENABLE() stub_clock_enable(1)

HAL_StatusTypeDef HAL_DAC_Init(DAC_HandleTypeDef *hdac);
HAL_StatusTypeDef HAL_DAC_ConfigChannel(DAC_HandleTypeDef *hdac, DAC_ChannelConfTypeDef *config, uint32_t channel);
HAL_StatusTypeDef HAL_DAC_SetValue(DAC_HandleTypeDef *hdac, uint32_t channel, uint32_t alignment, uint32_t data);
HAL_StatusTypeDef HAL_DAC_Start(DAC_HandleTypeDef *hdac, uint32_t channel);

#endif /* HYDRA_UMC_HOST_STUB_H7_DAC_HAL_H */
