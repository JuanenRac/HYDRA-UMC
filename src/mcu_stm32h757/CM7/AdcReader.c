/* =============================================================================
 * HYDRA-UMC - src/mcu_stm32h757/CM7/AdcReader.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Polled reading of ADC1 to ADC3 on the CM7; see AdcReader.h and Common/adc_plan.h.
 * ========================================================================== */
#include "AdcReader.h"

#if HYDRA_ADC_CM7

static ADC_HandleTypeDef g_adc[ADCP_ADC_MAX];
static int g_open[ADCP_ADC_MAX];

static const uint32_t g_channels[ADCP_CHANNEL_MAX + 1u] = {
    ADC_CHANNEL_0,  ADC_CHANNEL_1,  ADC_CHANNEL_2,  ADC_CHANNEL_3,  ADC_CHANNEL_4,  ADC_CHANNEL_5,  ADC_CHANNEL_6,  ADC_CHANNEL_7,  ADC_CHANNEL_8,  ADC_CHANNEL_9,
    ADC_CHANNEL_10, ADC_CHANNEL_11, ADC_CHANNEL_12, ADC_CHANNEL_13, ADC_CHANNEL_14, ADC_CHANNEL_15, ADC_CHANNEL_16, ADC_CHANNEL_17, ADC_CHANNEL_18, ADC_CHANNEL_19,
};

static const uint32_t g_sampling[ADCP_SAMPLE_CODE_MAX + 1u] = {ADC_SAMPLETIME_16CYCLES_5, ADC_SAMPLETIME_64CYCLES_5, ADC_SAMPLETIME_387CYCLES_5};

static ADC_TypeDef *AdcReader_Instance(uint8_t adc)
{
    return adc == 1u ? ADC1 : (adc == 2u ? ADC2 : ADC3);
}

int AdcReader_Open(uint8_t adc, const AdcpConfig *config)
{
    if (adc < 1u || adc > ADCP_ADC_MAX || AdcPlan_CheckConfig(config) != ADCP_OK) return ADCR_E_ARG;
    if (adc == 3u) {
        __HAL_RCC_ADC3_CLK_ENABLE();
    } else {
        __HAL_RCC_ADC12_CLK_ENABLE();
    }
    ADC_HandleTypeDef *h = &g_adc[adc - 1u];
    h->Instance = AdcReader_Instance(adc);
    h->Init.ClockPrescaler = ADC_CLOCK_ASYNC_DIV4;
    h->Init.Resolution = config->bits == 16u ? ADC_RESOLUTION_16B : ADC_RESOLUTION_12B;
    h->Init.ScanConvMode = ADC_SCAN_DISABLE;
    h->Init.EOCSelection = ADC_EOC_SINGLE_CONV;
    h->Init.LowPowerAutoWait = DISABLE;
    h->Init.ContinuousConvMode = DISABLE;
    h->Init.NbrOfConversion = 1u;
    h->Init.DiscontinuousConvMode = DISABLE;
    h->Init.ExternalTrigConv = ADC_SOFTWARE_START;
    h->Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
    h->Init.ConversionDataManagement = ADC_CONVERSIONDATA_DR;
    h->Init.Overrun = ADC_OVR_DATA_OVERWRITTEN;
    h->Init.LeftBitShift = ADC_LEFTBITSHIFT_NONE;
    h->Init.OversamplingMode = DISABLE;
    g_open[adc - 1u] = 0;
    if (HAL_ADC_Init(h) != HAL_OK) return ADCR_E_HAL;
    if (HAL_ADCEx_Calibration_Start(h, ADC_CALIB_OFFSET, ADC_SINGLE_ENDED) != HAL_OK) return ADCR_E_HAL;
    g_open[adc - 1u] = 1;
    return ADCR_OK;
}

int AdcReader_ReadRaw(const AdcpConfig *config, const AdcpChannel *channel, uint32_t *raw)
{
    if (raw == NULL || AdcPlan_CheckConfig(config) != ADCP_OK || AdcPlan_CheckChannel(channel) != ADCP_OK) return ADCR_E_ARG;
    if (!g_open[channel->adc - 1u]) return ADCR_E_NOT_OPEN;
    ADC_HandleTypeDef *h = &g_adc[channel->adc - 1u];
    ADC_ChannelConfTypeDef setup = {0};
    setup.Channel = g_channels[channel->channel];
    setup.Rank = ADC_REGULAR_RANK_1;
    setup.SamplingTime = g_sampling[config->sample_code];
    setup.SingleDiff = ADC_SINGLE_ENDED;
    setup.OffsetNumber = ADC_OFFSET_NONE;
    setup.Offset = 0u;
    if (HAL_ADC_ConfigChannel(h, &setup) != HAL_OK) return ADCR_E_HAL;
    uint32_t sum = 0u;
    for (uint32_t i = 0u; i < config->samples; i++) {
        if (HAL_ADC_Start(h) != HAL_OK) return ADCR_E_HAL;
        const HAL_StatusTypeDef done = HAL_ADC_PollForConversion(h, config->timeout_ms);
        if (done != HAL_OK) {
            (void)HAL_ADC_Stop(h);
            return done == HAL_TIMEOUT ? ADCR_E_TIMEOUT : ADCR_E_HAL;
        }
        sum += HAL_ADC_GetValue(h);
        (void)HAL_ADC_Stop(h);
    }
    *raw = AdcPlan_Average(sum, config->samples);
    return ADCR_OK;
}

int AdcReader_ReadMillivolts(const AdcpConfig *config, const AdcpChannel *channel, int32_t *mv)
{
    uint32_t raw = 0u;
    const int rc = AdcReader_ReadRaw(config, channel, &raw);
    if (rc != ADCR_OK) return rc;
    return AdcPlan_ToMillivolts(config, channel, raw, mv) == ADCP_OK ? ADCR_OK : ADCR_E_RANGE; /* the config and the channel were checked: only a result that does not fit is left */
}

#endif /* HYDRA_ADC_CM7 */
