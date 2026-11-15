/* =============================================================================
 * HYDRA-UMC - src/mcu_stm32h757/CM7/Fdcan1Start.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Start-up of FDCAN1 on the CM7; see Fdcan1Start.h and Common/fdcan_plan.h. The settings that are not in the plan are the ones of the CM4 of the H745 board: Classic CAN, normal
 * mode, automatic retransmission on, standard ids only, RX FIFO0 and a transmit FIFO with 8-byte elements.
 * ========================================================================== */
#include "Fdcan1Start.h"

#if HYDRA_FDCAN1_START

uint32_t Fdcan1Start_Run(FDCAN_HandleTypeDef *hfdcan, const FdcpPlan *plan)
{
    if (hfdcan == NULL || hfdcan->Instance == NULL || plan == NULL || plan->filter_count < 1u || plan->filter_count > 2u) return FDS_FAULT_ARG;

    hfdcan->Init.FrameFormat = FDCAN_FRAME_CLASSIC;
    hfdcan->Init.Mode = FDCAN_MODE_NORMAL;
    hfdcan->Init.AutoRetransmission = ENABLE;
    hfdcan->Init.TransmitPause = DISABLE;
    hfdcan->Init.ProtocolException = DISABLE;
    hfdcan->Init.NominalPrescaler = plan->prescaler;
    hfdcan->Init.NominalSyncJumpWidth = plan->sjw;
    hfdcan->Init.NominalTimeSeg1 = plan->seg1;
    hfdcan->Init.NominalTimeSeg2 = plan->seg2;
    hfdcan->Init.DataPrescaler = plan->prescaler; /* Classic CAN: the data phase is never used, the same numbers as the CM4 */
    hfdcan->Init.DataSyncJumpWidth = plan->sjw;
    hfdcan->Init.DataTimeSeg1 = plan->seg1;
    hfdcan->Init.DataTimeSeg2 = plan->seg2;
    hfdcan->Init.MessageRAMOffset = plan->ram_offset;
    hfdcan->Init.StdFiltersNbr = plan->filter_count;
    hfdcan->Init.ExtFiltersNbr = 0;
    hfdcan->Init.RxFifo0ElmtsNbr = plan->rx_elements;
    hfdcan->Init.RxFifo0ElmtSize = FDCAN_DATA_BYTES_8;
    hfdcan->Init.RxFifo1ElmtsNbr = 0;
    hfdcan->Init.RxBuffersNbr = 0;
    hfdcan->Init.TxEventsNbr = 0;
    hfdcan->Init.TxBuffersNbr = 0;
    hfdcan->Init.TxFifoQueueElmtsNbr = plan->tx_elements;
    hfdcan->Init.TxFifoQueueMode = FDCAN_TX_FIFO_OPERATION;
    hfdcan->Init.TxElmtSize = FDCAN_DATA_BYTES_8;
    if (HAL_FDCAN_Init(hfdcan) != HAL_OK) return FDS_FAULT_INIT; /* not usable: no filter, no start */

    uint32_t faults = FDS_OK;
    for (uint32_t i = 0; i < plan->filter_count; i++) {
        FDCAN_FilterTypeDef filter;
        filter.IdType = FDCAN_STANDARD_ID;
        filter.FilterIndex = i;
        filter.FilterType = FDCAN_FILTER_MASK;
        filter.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
        filter.FilterID1 = plan->filter[i].id;
        filter.FilterID2 = plan->filter[i].mask;
        if (HAL_FDCAN_ConfigFilter(hfdcan, &filter) != HAL_OK) faults |= FDS_FAULT_FILTER;
    }
    if (HAL_FDCAN_ConfigGlobalFilter(hfdcan, FDCAN_REJECT, FDCAN_REJECT, FDCAN_REJECT_REMOTE, FDCAN_REJECT_REMOTE) != HAL_OK) faults |= FDS_FAULT_FILTER;
    if (faults != FDS_OK) return faults; /* never start with a filter that is not the one that was asked for */

    return HAL_FDCAN_Start(hfdcan) == HAL_OK ? FDS_OK : FDS_FAULT_START;
}

int Fdcan1Start_ErrorCounters(void *ctx, uint8_t *tec, uint8_t *rec)
{
    FDCAN_ErrorCountersTypeDef counters;
    if (ctx == NULL || HAL_FDCAN_GetErrorCounters((FDCAN_HandleTypeDef *)ctx, &counters) != HAL_OK) return 0;
    *tec = (uint8_t)counters.TxErrorCnt;
    *rec = (uint8_t)counters.RxErrorCnt;
    return 1;
}

#endif /* HYDRA_FDCAN1_START */
