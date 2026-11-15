/* =============================================================================
 * HYDRA-UMC Firmware - tests/test_dac_out.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Host test of the analog outputs of the CM7 (src/mcu_stm32h757/CM7/DacOut.c) against a double of the HAL (tests/stubs/h7dac): the start-up (the clock, DAC1, the channel set-up, and
 * for each output the order 0 V first and then start), a refusing HAL at each step leaving the converter closed, a request before the start, the codes that reach the converter and
 * the requests that are refused without writing anything (over the ceiling, a channel that does not exist). Evidence: simulator.
 *
 *   cc -std=c11 -Wall -Wextra -DHYDRA_DAC_CM7=1 -Itests/stubs/h7dac -Isrc/mcu_stm32h757/CM7 -Isrc/mcu_stm32h757/Common \
 *      -o test_dac_out tests/test_dac_out.c src/mcu_stm32h757/CM7/DacOut.c
 */
#include <stdio.h>
#include <string.h>

#include "DacOut.h"

#if !HYDRA_DAC_CM7
#error "this test is for the outputs with their switch on: build it with -DHYDRA_DAC_CM7=1"
#endif

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } } while (0)

DAC_TypeDef stub_dac1;

static char calls[32];
static unsigned call_count, clock_calls;
static DAC_TypeDef *instance_seen;
static DAC_ChannelConfTypeDef setup_seen;
static uint32_t value_channel[8], value_data[8], value_align[8];
static unsigned value_count;
static int fail_init, fail_config, fail_set_at, fail_start; /* fail_set_at: the set (1-based) that fails */

static void note(char c) { if (call_count < sizeof calls - 1u) calls[call_count++] = c; }

void stub_clock_enable(int which) { (void)which; clock_calls++; }
HAL_StatusTypeDef HAL_DAC_Init(DAC_HandleTypeDef *h) { note('I'); instance_seen = h->Instance; return fail_init ? HAL_ERROR : HAL_OK; }
HAL_StatusTypeDef HAL_DAC_ConfigChannel(DAC_HandleTypeDef *h, DAC_ChannelConfTypeDef *c, uint32_t ch) { (void)h; (void)ch; note('C'); setup_seen = *c; return fail_config ? HAL_ERROR : HAL_OK; }
HAL_StatusTypeDef HAL_DAC_SetValue(DAC_HandleTypeDef *h, uint32_t ch, uint32_t align, uint32_t data)
{
    (void)h;
    note('V');
    if (value_count < 8u) { value_channel[value_count] = ch; value_align[value_count] = align; value_data[value_count] = data; }
    value_count++;
    return (fail_set_at && value_count == (unsigned)fail_set_at) ? HAL_ERROR : HAL_OK;
}
HAL_StatusTypeDef HAL_DAC_Start(DAC_HandleTypeDef *h, uint32_t ch) { (void)h; (void)ch; note('S'); return fail_start ? HAL_ERROR : HAL_OK; }

static void reset(void)
{
    memset(calls, 0, sizeof calls);
    call_count = clock_calls = value_count = 0u;
    fail_init = fail_config = fail_set_at = fail_start = 0;
}

int main(void)
{
    DacpConfig config = {3300u, 3u, 1u, {9900u, 5000u}};

    /* before the start */
    reset();
    CHECK(DacOut_SetMillivolts(1u, 100u) == DACO_E_NOT_OPEN && value_count == 0u, "a request before the start writes nothing");

    /* bad configuration, nothing touched */
    DacpConfig bad = config;
    bad.gain_num = 0u;
    CHECK(DacOut_Open(NULL) == DACO_E_ARG && DacOut_Open(&bad) == DACO_E_ARG && call_count == 0u && clock_calls == 0u, "a configuration the plan refuses touches nothing");

    /* the start */
    reset();
    CHECK(DacOut_Open(&config) == DACO_OK && instance_seen == DAC1 && clock_calls == 1u, "DAC1 starts with its clock");
    CHECK(strcmp(calls, "ICVSCVS") == 0, "the order: init, then for each output set-up, 0 V, start");
    CHECK(value_count == 2u && value_data[0] == 0u && value_data[1] == 0u && value_channel[0] == DAC_CHANNEL_1 && value_channel[1] == DAC_CHANNEL_2 && value_align[0] == DAC_ALIGN_12B_R, "both outputs are at code 0 before they start");
    CHECK(setup_seen.DAC_Trigger == DAC_TRIGGER_NONE && setup_seen.DAC_OutputBuffer == DAC_OUTPUTBUFFER_ENABLE && setup_seen.DAC_ConnectOnChipPeripheral == DAC_CHIPCONNECT_EXTERNAL && setup_seen.DAC_SampleAndHold == DAC_SAMPLEANDHOLD_DISABLE && setup_seen.DAC_UserTrimming == DAC_TRIMMING_FACTORY, "no trigger, buffer on, to the pin");

    /* the codes */
    value_count = 0u;
    CHECK(DacOut_SetMillivolts(1u, 9900u) == DACO_OK && value_channel[0] == DAC_CHANNEL_1 && value_data[0] == 4095u, "9.9 V on output 1 is 4095");
    CHECK(DacOut_SetMillivolts(2u, 2475u) == DACO_OK && value_channel[1] == DAC_CHANNEL_2 && value_data[1] == 1024u, "2.475 V on output 2 is 1024 (1023.75)");
    const unsigned written = value_count;
    CHECK(DacOut_SetMillivolts(2u, 5001u) == DACO_E_RANGE && DacOut_SetMillivolts(1u, 9901u) == DACO_E_RANGE && DacOut_SetMillivolts(0u, 1u) == DACO_E_CHANNEL && DacOut_SetMillivolts(3u, 1u) == DACO_E_CHANNEL && value_count == written, "refused requests write nothing");

    /* a refusing HAL leaves the converter closed */
    reset(); fail_init = 1;
    CHECK(DacOut_Open(&config) == DACO_E_HAL && strcmp(calls, "I") == 0 && DacOut_SetMillivolts(1u, 0u) == DACO_E_NOT_OPEN, "a refused init stops there, closed");
    reset(); fail_config = 1;
    CHECK(DacOut_Open(&config) == DACO_E_HAL && strcmp(calls, "IC") == 0, "a refused channel set-up stops before any value");
    reset(); fail_set_at = 1;
    CHECK(DacOut_Open(&config) == DACO_E_HAL && strcmp(calls, "ICV") == 0, "a refused 0 V stops before the output starts");
    reset(); fail_start = 1;
    CHECK(DacOut_Open(&config) == DACO_E_HAL && strcmp(calls, "ICVS") == 0 && DacOut_SetMillivolts(1u, 0u) == DACO_E_NOT_OPEN, "a refused start leaves it closed");

    /* a HAL that refuses a value after the start */
    reset();
    CHECK(DacOut_Open(&config) == DACO_OK, "open again");
    fail_set_at = 3;
    CHECK(DacOut_SetMillivolts(1u, 100u) == DACO_E_HAL, "a refused value is reported");

    if (failures) {
        printf("dac_out host tests: %d FAILED\n", failures);
        return 1;
    }
    printf("dac_out host logic tests passed\n");
    return 0;
}
