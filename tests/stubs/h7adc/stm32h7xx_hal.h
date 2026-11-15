/* =============================================================================
 * HYDRA-UMC Firmware - Host test stand-in for the STM32H7 HAL header (ADC)
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Just enough of the vendor header for the ADC reader of the CM7 (src/mcu_stm32h757/CM7/AdcReader.c) to compile with the host's plain gcc. The names and the fields are the ones of
 * the HAL of the H7; the functions are declared here and defined by the test, which plays the ADCs and records what the driver did.
 */
#ifndef HYDRA_UMC_HOST_STUB_H7_ADC_HAL_H
#define HYDRA_UMC_HOST_STUB_H7_ADC_HAL_H

#include <stddef.h>
#include <stdint.h>

typedef enum { HAL_OK = 0, HAL_ERROR = 1, HAL_BUSY = 2, HAL_TIMEOUT = 3 } HAL_StatusTypeDef;

#define DISABLE 0U
#define ENABLE 1U

typedef struct { int instance_id; } ADC_TypeDef;

typedef struct {
    uint32_t ClockPrescaler, Resolution, ScanConvMode, EOCSelection, LowPowerAutoWait, ContinuousConvMode, NbrOfConversion, DiscontinuousConvMode;
    uint32_t ExternalTrigConv, ExternalTrigConvEdge, ConversionDataManagement, Overrun, LeftBitShift, OversamplingMode;
} ADC_InitTypeDef;

typedef struct {
    ADC_TypeDef *Instance;
    ADC_InitTypeDef Init;
} ADC_HandleTypeDef;

typedef struct { uint32_t Channel, Rank, SamplingTime, SingleDiff, OffsetNumber, Offset; } ADC_ChannelConfTypeDef;

extern ADC_TypeDef stub_adc1, stub_adc2, stub_adc3;
#define ADC1 (&stub_adc1)
#define ADC2 (&stub_adc2)
#define ADC3 (&stub_adc3)

#define ADC_CHANNEL_0 0x100U
#define ADC_CHANNEL_1 0x101U
#define ADC_CHANNEL_2 0x102U
#define ADC_CHANNEL_3 0x103U
#define ADC_CHANNEL_4 0x104U
#define ADC_CHANNEL_5 0x105U
#define ADC_CHANNEL_6 0x106U
#define ADC_CHANNEL_7 0x107U
#define ADC_CHANNEL_8 0x108U
#define ADC_CHANNEL_9 0x109U
#define ADC_CHANNEL_10 0x10AU
#define ADC_CHANNEL_11 0x10BU
#define ADC_CHANNEL_12 0x10CU
#define ADC_CHANNEL_13 0x10DU
#define ADC_CHANNEL_14 0x10EU
#define ADC_CHANNEL_15 0x10FU
#define ADC_CHANNEL_16 0x110U
#define ADC_CHANNEL_17 0x111U
#define ADC_CHANNEL_18 0x112U
#define ADC_CHANNEL_19 0x113U

#define ADC_CLOCK_ASYNC_DIV4 0x00040000U
#define ADC_RESOLUTION_16B 0U
#define ADC_RESOLUTION_12B 0x14U
#define ADC_SCAN_DISABLE 0U
#define ADC_EOC_SINGLE_CONV 8U
#define ADC_SOFTWARE_START 1U
#define ADC_EXTERNALTRIGCONVEDGE_NONE 0U
#define ADC_CONVERSIONDATA_DR 0U
#define ADC_OVR_DATA_OVERWRITTEN 0x1000U
#define ADC_LEFTBITSHIFT_NONE 0U
#define ADC_CALIB_OFFSET 0U
#define ADC_SINGLE_ENDED 0U
#define ADC_REGULAR_RANK_1 1U
#define ADC_SAMPLETIME_16CYCLES_5 3U
#define ADC_SAMPLETIME_64CYCLES_5 4U
#define ADC_SAMPLETIME_387CYCLES_5 6U
#define ADC_OFFSET_NONE 0U

void stub_clock_enable(int which);
#define __HAL_RCC_ADC12_CLK_ENABLE() stub_clock_enable(1)
#define __HAL_RCC_ADC3_CLK_ENABLE() stub_clock_enable(2)

HAL_StatusTypeDef HAL_ADC_Init(ADC_HandleTypeDef *hadc);
HAL_StatusTypeDef HAL_ADCEx_Calibration_Start(ADC_HandleTypeDef *hadc, uint32_t calibration_mode, uint32_t single_diff);
HAL_StatusTypeDef HAL_ADC_ConfigChannel(ADC_HandleTypeDef *hadc, ADC_ChannelConfTypeDef *config);
HAL_StatusTypeDef HAL_ADC_Start(ADC_HandleTypeDef *hadc);
HAL_StatusTypeDef HAL_ADC_PollForConversion(ADC_HandleTypeDef *hadc, uint32_t timeout);
uint32_t HAL_ADC_GetValue(ADC_HandleTypeDef *hadc);
HAL_StatusTypeDef HAL_ADC_Stop(ADC_HandleTypeDef *hadc);

#endif /* HYDRA_UMC_HOST_STUB_H7_ADC_HAL_H */
