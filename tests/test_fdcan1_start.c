/* =============================================================================
 * HYDRA-UMC Firmware - tests/test_fdcan1_start.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Host test of the start-up of FDCAN1 on the CM7 (src/mcu_stm32h757/CM7/Fdcan1Start.c) against a double of the HAL (tests/stubs/h7fdcanstart): the double records the calls and the
 * Init structure it was given. It checks the numbers that reach the peripheral (timing, FIFOs, Classic CAN, 8-byte elements), the order of the calls (initialise, filters, global
 * filter, start), the refusal at each step (an initialisation that fails stops before the filters, a filter that fails stops before the start, a start that fails is reported), the
 * refusal of bad arguments, and the reading of the error counters. Evidence: simulator.
 *
 *   cc -std=c11 -Wall -Wextra -DHYDRA_FDCAN1_START=1 -Itests/stubs/h7fdcanstart -Isrc/mcu_stm32h757/CM7 -Isrc/mcu_stm32h757/Common \
 *      -o test_fdcan1_start tests/test_fdcan1_start.c src/mcu_stm32h757/CM7/Fdcan1Start.c
 */
#include <stdio.h>
#include <string.h>

#include "Fdcan1Start.h"

#if !HYDRA_FDCAN1_START
#error "this test is for the start-up with its switch on: build it with -DHYDRA_FDCAN1_START=1"
#endif

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } } while (0)

static char calls[16];
static unsigned call_count;
static FDCAN_InitTypeDef init_seen;
static FDCAN_FilterTypeDef filters_seen[4];
static unsigned filter_count_seen;
static uint32_t global_seen[4];
static int fail_init, fail_filter_at, fail_global, fail_start, fail_counters;
static uint32_t tec_value, rec_value;

static void note(char c) { if (call_count < sizeof calls - 1u) calls[call_count++] = c; }

HAL_StatusTypeDef HAL_FDCAN_Init(FDCAN_HandleTypeDef *h)
{
    note('I');
    init_seen = h->Init;
    return fail_init ? HAL_ERROR : HAL_OK;
}

HAL_StatusTypeDef HAL_FDCAN_ConfigFilter(FDCAN_HandleTypeDef *h, FDCAN_FilterTypeDef *f)
{
    (void)h;
    note('F');
    if (filter_count_seen < 4u) filters_seen[filter_count_seen] = *f;
    filter_count_seen++;
    return (fail_filter_at && filter_count_seen == (unsigned)fail_filter_at) ? HAL_ERROR : HAL_OK;
}

HAL_StatusTypeDef HAL_FDCAN_ConfigGlobalFilter(FDCAN_HandleTypeDef *h, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    (void)h;
    note('G');
    global_seen[0] = a; global_seen[1] = b; global_seen[2] = c; global_seen[3] = d;
    return fail_global ? HAL_ERROR : HAL_OK;
}

HAL_StatusTypeDef HAL_FDCAN_Start(FDCAN_HandleTypeDef *h)
{
    (void)h;
    note('S');
    return fail_start ? HAL_ERROR : HAL_OK;
}

HAL_StatusTypeDef HAL_FDCAN_GetErrorCounters(FDCAN_HandleTypeDef *h, FDCAN_ErrorCountersTypeDef *counters)
{
    (void)h;
    if (fail_counters) return HAL_ERROR;
    counters->TxErrorCnt = tec_value;
    counters->RxErrorCnt = rec_value;
    return HAL_OK;
}

static void reset(void)
{
    memset(calls, 0, sizeof calls);
    call_count = 0u;
    memset(&init_seen, 0, sizeof init_seen);
    memset(filters_seen, 0, sizeof filters_seen);
    filter_count_seen = 0u;
    memset(global_seen, 0, sizeof global_seen);
    fail_init = fail_filter_at = fail_global = fail_start = fail_counters = 0;
}

int main(void)
{
    FDCAN_GlobalTypeDef instance = {1};
    FDCAN_HandleTypeDef handle;
    FdcpConfig config;
    FdcpPlan plan;

    FdcanPlan_Defaults(&config);
    CHECK(FdcanPlan_Make(&config, &plan) == FDCP_OK, "a plan to start with");

    /* the good start: the numbers, the order, the filters */
    reset();
    memset(&handle, 0, sizeof handle);
    handle.Instance = &instance;
    CHECK(Fdcan1Start_Run(&handle, &plan) == FDS_OK, "a good start");
    CHECK(strcmp(calls, "IFFGS") == 0, "initialise, two filters, global filter, start");
    CHECK(init_seen.NominalPrescaler == 5u && init_seen.NominalTimeSeg1 == 16u && init_seen.NominalTimeSeg2 == 3u && init_seen.NominalSyncJumpWidth == 3u, "the nominal timing of the plan");
    CHECK(init_seen.DataPrescaler == 5u && init_seen.DataTimeSeg1 == 16u && init_seen.DataTimeSeg2 == 3u && init_seen.DataSyncJumpWidth == 3u, "the same numbers for the data phase");
    CHECK(init_seen.FrameFormat == FDCAN_FRAME_CLASSIC && init_seen.Mode == FDCAN_MODE_NORMAL && init_seen.AutoRetransmission == ENABLE && init_seen.TransmitPause == DISABLE && init_seen.ProtocolException == DISABLE, "Classic CAN in normal mode");
    CHECK(init_seen.StdFiltersNbr == 2u && init_seen.ExtFiltersNbr == 0u && init_seen.RxFifo0ElmtsNbr == 8u && init_seen.RxFifo0ElmtSize == FDCAN_DATA_BYTES_8, "filters and receive FIFO");
    CHECK(init_seen.RxFifo1ElmtsNbr == 0u && init_seen.RxBuffersNbr == 0u && init_seen.TxEventsNbr == 0u && init_seen.TxBuffersNbr == 0u && init_seen.MessageRAMOffset == 0u, "nothing else is allocated");
    CHECK(init_seen.TxFifoQueueElmtsNbr == 8u && init_seen.TxFifoQueueMode == FDCAN_TX_FIFO_OPERATION && init_seen.TxElmtSize == FDCAN_DATA_BYTES_8, "transmit FIFO");
    CHECK(filter_count_seen == 2u && filters_seen[0].FilterIndex == 0u && filters_seen[0].FilterID1 == 0x600u && filters_seen[0].FilterID2 == 0x600u, "the window of the boards");
    CHECK(filters_seen[1].FilterIndex == 1u && filters_seen[1].FilterID1 == 0x001u && filters_seen[1].FilterID2 == 0x7FFu, "the emergency stop");
    CHECK(filters_seen[0].IdType == FDCAN_STANDARD_ID && filters_seen[0].FilterType == FDCAN_FILTER_MASK && filters_seen[1].FilterConfig == FDCAN_FILTER_TO_RXFIFO0, "standard ids, mask, into FIFO0");
    CHECK(global_seen[0] == FDCAN_REJECT && global_seen[1] == FDCAN_REJECT && global_seen[2] == FDCAN_REJECT_REMOTE && global_seen[3] == FDCAN_REJECT_REMOTE, "everything else is rejected");

    /* one filter only */
    reset();
    config.estop_filter = 0u;
    CHECK(FdcanPlan_Make(&config, &plan) == FDCP_OK && Fdcan1Start_Run(&handle, &plan) == FDS_OK && strcmp(calls, "IFGS") == 0 && init_seen.StdFiltersNbr == 1u, "one filter");

    /* each step that is refused */
    reset();
    fail_init = 1;
    CHECK(Fdcan1Start_Run(&handle, &plan) == FDS_FAULT_INIT && strcmp(calls, "I") == 0, "a refused initialisation stops there");
    reset();
    fail_filter_at = 1;
    CHECK(Fdcan1Start_Run(&handle, &plan) == FDS_FAULT_FILTER && strchr(calls, 'S') == NULL, "a refused filter is not started");
    reset();
    config.estop_filter = 1u;
    CHECK(FdcanPlan_Make(&config, &plan) == FDCP_OK, "a plan with two filters again");
    fail_filter_at = 2;
    CHECK(Fdcan1Start_Run(&handle, &plan) == FDS_FAULT_FILTER && strcmp(calls, "IFFG") == 0, "the second filter refused: every step still runs, but no start");
    reset();
    fail_global = 1;
    CHECK(Fdcan1Start_Run(&handle, &plan) == FDS_FAULT_FILTER && strchr(calls, 'S') == NULL, "a refused global filter is not started");
    reset();
    fail_start = 1;
    CHECK(Fdcan1Start_Run(&handle, &plan) == FDS_FAULT_START && strcmp(calls, "IFFGS") == 0, "a start that fails is reported");

    /* the second controller: the same start-up with a plan placed after the first */
    {
        FdcpPlan field;
        FDCAN_GlobalTypeDef instance2 = {2};
        FDCAN_HandleTypeDef handle2;
        reset();
        config.estop_filter = 1u;
        CHECK(FdcanPlan_Make(&config, &plan) == FDCP_OK && FdcanPlan_MakeField(&config, 0u, 0u, &plan, &field) == FDCP_OK, "plans for both controllers");
        memset(&handle2, 0, sizeof handle2);
        handle2.Instance = &instance2;
        CHECK(Fdcan1Start_Run(&handle2, &field) == FDS_OK && strcmp(calls, "IFGS") == 0, "FDCAN2 starts with its one filter");
        CHECK(init_seen.MessageRAMOffset == 66u && init_seen.StdFiltersNbr == 1u && filters_seen[0].FilterID1 == 0u && filters_seen[0].FilterID2 == 0u, "its place in the message RAM and its filter");
        reset();
        CHECK(Fdcan1Start_Run(&handle, &plan) == FDS_OK && init_seen.MessageRAMOffset == 0u, "FDCAN1 stays at 0");
    }

    /* arguments */
    reset();
    CHECK(Fdcan1Start_Run(NULL, &plan) == FDS_FAULT_ARG && Fdcan1Start_Run(&handle, NULL) == FDS_FAULT_ARG, "no handle, no plan");
    FDCAN_HandleTypeDef bare;
    memset(&bare, 0, sizeof bare);
    CHECK(Fdcan1Start_Run(&bare, &plan) == FDS_FAULT_ARG, "no instance");
    FdcpPlan bad = plan;
    bad.filter_count = 0u;
    CHECK(Fdcan1Start_Run(&handle, &bad) == FDS_FAULT_ARG, "no filter");
    bad.filter_count = 3u;
    CHECK(Fdcan1Start_Run(&handle, &bad) == FDS_FAULT_ARG && call_count == 0u, "three filters: nothing touched");

    /* the counters */
    uint8_t tec = 0xEEu, rec = 0xEEu;
    tec_value = 9u; rec_value = 4u;
    CHECK(Fdcan1Start_ErrorCounters(&handle, &tec, &rec) == 1 && tec == 9u && rec == 4u, "the counters are read");
    fail_counters = 1;
    tec = rec = 0xEEu;
    CHECK(Fdcan1Start_ErrorCounters(&handle, &tec, &rec) == 0 && tec == 0xEEu && rec == 0xEEu, "a failing read leaves them alone");
    CHECK(Fdcan1Start_ErrorCounters(NULL, &tec, &rec) == 0, "no handle");

    if (failures) {
        printf("fdcan1_start host tests: %d FAILED\n", failures);
        return 1;
    }
    printf("fdcan1_start host logic tests passed\n");
    return 0;
}
