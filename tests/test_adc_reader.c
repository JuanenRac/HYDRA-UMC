/* =============================================================================
 * HYDRA-UMC Firmware - tests/test_adc_reader.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Host test of the ADC reader of the CM7 (src/mcu_stm32h757/CM7/AdcReader.c) against a double of the HAL (tests/stubs/h7adc) in which the double plays the three ADCs and counts the
 * calls: what the start-up wrote (clock of ADC1/2 and ADC3, resolution, software trigger, single conversion, calibration), the refusal of a bad ADC, configuration or channel before
 * the HAL is called, a refusing HAL at each step, the channel set-up (the right channel macro, rank 1, the sampling time of the code), the average of the conversions, a conversion
 * that times out or fails (the ADC is stopped), a reading of an ADC that is not open, and the reading in millivolts. Evidence: simulator.
 *
 *   cc -std=c11 -Wall -Wextra -DHYDRA_ADC_CM7=1 -Itests/stubs/h7adc -Isrc/mcu_stm32h757/CM7 -Isrc/mcu_stm32h757/Common \
 *      -o test_adc_reader tests/test_adc_reader.c src/mcu_stm32h757/CM7/AdcReader.c
 */
#include <stdio.h>
#include <string.h>

#include "AdcReader.h"

#if !HYDRA_ADC_CM7
#error "this test is for the reader with its switch on: build it with -DHYDRA_ADC_CM7=1"
#endif

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } } while (0)

ADC_TypeDef stub_adc1, stub_adc2, stub_adc3;

static int clocks[3];
static unsigned init_calls, cal_calls, config_calls, start_calls, poll_calls, stop_calls;
static ADC_InitTypeDef init_seen;
static ADC_TypeDef *init_instance, *config_instance;
static ADC_ChannelConfTypeDef channel_seen;
static int fail_init, fail_cal, fail_config, fail_start, poll_result_at, poll_result; /* poll_result_at: the poll (1-based) that gets poll_result */
static unsigned polls_done;
static uint32_t values[8], value_index;

void stub_clock_enable(int which) { if (which >= 0 && which < 3) clocks[which]++; }
HAL_StatusTypeDef HAL_ADC_Init(ADC_HandleTypeDef *h) { init_calls++; init_instance = h->Instance; init_seen = h->Init; return fail_init ? HAL_ERROR : HAL_OK; }
HAL_StatusTypeDef HAL_ADCEx_Calibration_Start(ADC_HandleTypeDef *h, uint32_t mode, uint32_t sd) { (void)h; (void)mode; (void)sd; cal_calls++; return fail_cal ? HAL_ERROR : HAL_OK; }
HAL_StatusTypeDef HAL_ADC_ConfigChannel(ADC_HandleTypeDef *h, ADC_ChannelConfTypeDef *c) { config_calls++; config_instance = h->Instance; channel_seen = *c; return fail_config ? HAL_ERROR : HAL_OK; }
HAL_StatusTypeDef HAL_ADC_Start(ADC_HandleTypeDef *h) { (void)h; start_calls++; return fail_start ? HAL_ERROR : HAL_OK; }
HAL_StatusTypeDef HAL_ADC_PollForConversion(ADC_HandleTypeDef *h, uint32_t t) { (void)h; (void)t; poll_calls++; polls_done++; return (poll_result_at && polls_done == (unsigned)poll_result_at) ? (HAL_StatusTypeDef)poll_result : HAL_OK; }
uint32_t HAL_ADC_GetValue(ADC_HandleTypeDef *h) { (void)h; return values[value_index++ & 7u]; }
HAL_StatusTypeDef HAL_ADC_Stop(ADC_HandleTypeDef *h) { (void)h; stop_calls++; return HAL_OK; }

static void reset(void)
{
    memset(clocks, 0, sizeof clocks);
    init_calls = cal_calls = config_calls = start_calls = poll_calls = stop_calls = polls_done = 0u;
    fail_init = fail_cal = fail_config = fail_start = poll_result_at = poll_result = 0;
    value_index = 0u;
    memset(values, 0, sizeof values);
}

int main(void)
{
    AdcpConfig config = {3300u, 16u, 4u, 10u, 1u};
    AdcpChannel channel = {1u, 16u, 11u, 1u, 0};
    uint32_t raw = 0xDEADu;
    int32_t mv = 0x5A5A;

    /* before anything is open, and bad arguments: the HAL is not touched */
    reset();
    CHECK(AdcReader_ReadRaw(&config, &channel, &raw) == ADCR_E_NOT_OPEN && config_calls == 0u, "an ADC that was not started");
    CHECK(AdcReader_Open(0u, &config) == ADCR_E_ARG && AdcReader_Open(4u, &config) == ADCR_E_ARG && AdcReader_Open(1u, NULL) == ADCR_E_ARG && init_calls == 0u, "no ADC 0 or 4, no configuration");
    AdcpConfig bad = config;
    bad.bits = 10u;
    CHECK(AdcReader_Open(1u, &bad) == ADCR_E_ARG && init_calls == 0u, "a configuration the plan refuses");

    /* the start-up of ADC1: 16 bits */
    reset();
    CHECK(AdcReader_Open(1u, &config) == ADCR_OK && init_instance == ADC1 && clocks[1] == 1 && clocks[2] == 0, "ADC1 starts with the clock of ADC1 and 2");
    CHECK(init_seen.Resolution == ADC_RESOLUTION_16B && init_seen.ClockPrescaler == ADC_CLOCK_ASYNC_DIV4 && init_seen.ScanConvMode == ADC_SCAN_DISABLE && init_seen.EOCSelection == ADC_EOC_SINGLE_CONV, "16 bits, asynchronous clock, one conversion");
    CHECK(init_seen.ContinuousConvMode == DISABLE && init_seen.NbrOfConversion == 1u && init_seen.ExternalTrigConv == ADC_SOFTWARE_START && init_seen.ExternalTrigConvEdge == ADC_EXTERNALTRIGCONVEDGE_NONE, "software start, no continuous mode");
    CHECK(init_seen.ConversionDataManagement == ADC_CONVERSIONDATA_DR && init_seen.Overrun == ADC_OVR_DATA_OVERWRITTEN && init_seen.LeftBitShift == ADC_LEFTBITSHIFT_NONE && init_seen.OversamplingMode == DISABLE && init_seen.LowPowerAutoWait == DISABLE && init_seen.DiscontinuousConvMode == DISABLE, "data register, no shift, no oversampling");
    CHECK(cal_calls == 1u, "calibrated once");

    /* ADC3 and 12 bits */
    reset();
    config.bits = 12u;
    CHECK(AdcReader_Open(3u, &config) == ADCR_OK && init_instance == ADC3 && clocks[2] == 1 && clocks[1] == 0 && init_seen.Resolution == ADC_RESOLUTION_12B, "ADC3 has its own clock, 12 bits");
    reset();
    CHECK(AdcReader_Open(2u, &config) == ADCR_OK && init_instance == ADC2 && clocks[1] == 1, "ADC2 shares the clock of ADC1");
    config.bits = 16u;

    /* a refusing HAL leaves the ADC closed */
    reset(); fail_init = 1;
    CHECK(AdcReader_Open(1u, &config) == ADCR_E_HAL && cal_calls == 0u && AdcReader_ReadRaw(&config, &channel, &raw) == ADCR_E_NOT_OPEN, "a refused initialisation: no calibration, closed");
    reset(); fail_cal = 1;
    CHECK(AdcReader_Open(1u, &config) == ADCR_E_HAL && AdcReader_ReadRaw(&config, &channel, &raw) == ADCR_E_NOT_OPEN, "a failed calibration: closed");

    /* the reading: ADC1 is open, with 4 samples */
    reset();
    CHECK(AdcReader_Open(1u, &config) == ADCR_OK, "ADC1 open again");
    values[0] = 100u; values[1] = 101u; values[2] = 102u; values[3] = 103u;
    CHECK(AdcReader_ReadRaw(&config, &channel, &raw) == ADCR_OK && raw == 102u, "the average of 100, 101, 102, 103 to the nearest is 102");
    CHECK(config_instance == ADC1 && channel_seen.Channel == ADC_CHANNEL_16 && channel_seen.Rank == ADC_REGULAR_RANK_1 && channel_seen.SamplingTime == ADC_SAMPLETIME_64CYCLES_5 && channel_seen.SingleDiff == ADC_SINGLE_ENDED && channel_seen.OffsetNumber == ADC_OFFSET_NONE, "channel 16, rank 1, 64.5 cycles");
    CHECK(start_calls == 4u && poll_calls == 4u && stop_calls == 4u && config_calls == 1u, "four conversions, each started and stopped");
    config.sample_code = 0u; channel.channel = 0u;
    CHECK(AdcReader_ReadRaw(&config, &channel, &raw) == ADCR_OK && channel_seen.SamplingTime == ADC_SAMPLETIME_16CYCLES_5 && channel_seen.Channel == ADC_CHANNEL_0, "code 0 is 16.5 cycles");
    config.sample_code = 2u; channel.channel = 19u;
    CHECK(AdcReader_ReadRaw(&config, &channel, &raw) == ADCR_OK && channel_seen.SamplingTime == ADC_SAMPLETIME_387CYCLES_5 && channel_seen.Channel == ADC_CHANNEL_19, "code 2 is 387.5 cycles, channel 19");
    config.sample_code = 1u; channel.channel = 16u;

    /* refused before the HAL is called */
    const unsigned before = config_calls;
    AdcpChannel unassigned = channel;
    unassigned.adc = 0u;
    CHECK(AdcReader_ReadRaw(&config, &unassigned, &raw) == ADCR_E_ARG && AdcReader_ReadRaw(&config, &channel, NULL) == ADCR_E_ARG && AdcReader_ReadRaw(&bad, &channel, &raw) == ADCR_E_ARG && config_calls == before, "an unassigned input, no output, a bad configuration");
    AdcpChannel on2 = channel;
    on2.adc = 2u;
    fail_init = 1;
    CHECK(AdcReader_Open(2u, &config) == ADCR_E_HAL && AdcReader_ReadRaw(&config, &on2, &raw) == ADCR_E_NOT_OPEN, "ADC2 was closed by the failed start");
    fail_init = 0;

    /* HAL failures in a reading */
    fail_config = 1;
    CHECK(AdcReader_ReadRaw(&config, &channel, &raw) == ADCR_E_HAL, "a refused channel");
    fail_config = 0; fail_start = 1;
    CHECK(AdcReader_ReadRaw(&config, &channel, &raw) == ADCR_E_HAL, "a conversion that does not start");
    fail_start = 0;
    stop_calls = 0u; polls_done = 0u; poll_result_at = 2; poll_result = HAL_TIMEOUT;
    CHECK(AdcReader_ReadRaw(&config, &channel, &raw) == ADCR_E_TIMEOUT && stop_calls == 2u, "a time-out in the second conversion stops the ADC after the first one stopped it too");
    stop_calls = 0u; polls_done = 0u; poll_result_at = 1; poll_result = HAL_ERROR;
    CHECK(AdcReader_ReadRaw(&config, &channel, &raw) == ADCR_E_HAL && stop_calls == 1u, "an error in the first conversion stops the ADC");
    poll_result_at = 0;

    /* millivolts */
    value_index = 0u; values[0] = values[1] = values[2] = values[3] = 32768u;
    CHECK(AdcReader_ReadMillivolts(&config, &channel, &mv) == ADCR_OK && mv == 18150, "1650 mV at the pin through a divider of 11 is 18150 mV");
    value_index = 0u; values[0] = values[1] = values[2] = values[3] = 65535u;
    channel.num = 1000000UL;
    mv = 0x5A5A;
    CHECK(AdcReader_ReadMillivolts(&config, &channel, &mv) == ADCR_E_RANGE && mv == 0x5A5A, "a signal that does not fit is an error");
    channel.num = 11u;
    value_index = 0u;
    CHECK(AdcReader_ReadMillivolts(&config, &channel, NULL) == ADCR_E_RANGE || AdcReader_ReadMillivolts(&config, &channel, NULL) == ADCR_E_ARG, "no output is refused");
    CHECK(AdcReader_ReadMillivolts(&config, &unassigned, &mv) == ADCR_E_ARG, "the errors of the reading pass through");

    if (failures) {
        printf("adc_reader host tests: %d FAILED\n", failures);
        return 1;
    }
    printf("adc_reader host logic tests passed\n");
    return 0;
}
