/* =============================================================================
 * HYDRA-UMC - src/mcu_stm32h757/CM7/DacOut.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * DAC1 of the CM7 as the analog outputs; see DacOut.h and Common/dac_plan.h. The pins PA4 and PA5 stay in analog mode, which is how they come out of reset.
 * ========================================================================== */
#include "DacOut.h"

#if HYDRA_DAC_CM7

static DAC_HandleTypeDef g_dac;
static const DacpConfig *g_config;
static int g_open;

static const uint32_t g_channel[DACP_CHANNELS] = {DAC_CHANNEL_1, DAC_CHANNEL_2};

int DacOut_Open(const DacpConfig *config)
{
    if (DacPlan_CheckConfig(config) != DACP_OK) return DACO_E_ARG;
    g_open = 0;
    __HAL_RCC_DAC12_CLK_ENABLE();
    g_dac.Instance = DAC1;
    if (HAL_DAC_Init(&g_dac) != HAL_OK) return DACO_E_HAL;
    DAC_ChannelConfTypeDef setup = {0};
    setup.DAC_SampleAndHold = DAC_SAMPLEANDHOLD_DISABLE;
    setup.DAC_Trigger = DAC_TRIGGER_NONE;
    setup.DAC_OutputBuffer = DAC_OUTPUTBUFFER_ENABLE;
    setup.DAC_ConnectOnChipPeripheral = DAC_CHIPCONNECT_EXTERNAL;
    setup.DAC_UserTrimming = DAC_TRIMMING_FACTORY;
    for (unsigned i = 0u; i < DACP_CHANNELS; i++) {
        if (HAL_DAC_ConfigChannel(&g_dac, &setup, g_channel[i]) != HAL_OK) return DACO_E_HAL;
        if (HAL_DAC_SetValue(&g_dac, g_channel[i], DAC_ALIGN_12B_R, 0u) != HAL_OK) return DACO_E_HAL; /* 0 V before the output is switched on */
        if (HAL_DAC_Start(&g_dac, g_channel[i]) != HAL_OK) return DACO_E_HAL;
    }
    g_config = config;
    g_open = 1;
    return DACO_OK;
}

int DacOut_SetMillivolts(unsigned channel, uint32_t millivolts)
{
    if (!g_open) return DACO_E_NOT_OPEN;
    uint32_t code = 0u;
    const int rc = DacPlan_CodeFor(g_config, channel, millivolts, &code);
    if (rc == DACP_E_CHANNEL) return DACO_E_CHANNEL;
    if (rc != DACP_OK) return DACO_E_RANGE; /* the configuration was checked when the converter was opened: only the range is left */
    return HAL_DAC_SetValue(&g_dac, g_channel[channel - 1u], DAC_ALIGN_12B_R, code) == HAL_OK ? DACO_OK : DACO_E_HAL;
}

#endif /* HYDRA_DAC_CM7 */
