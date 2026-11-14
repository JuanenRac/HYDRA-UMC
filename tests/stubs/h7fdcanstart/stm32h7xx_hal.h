/* =============================================================================
 * HYDRA-UMC Firmware - Host test stand-in for the STM32H7 HAL header (FDCAN start-up)
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Just enough of the vendor header for the start-up of FDCAN1 (src/mcu_stm32h757/CM7/Fdcan1Start.c) to compile with the host's plain gcc. The names and the fields are the ones of the
 * HAL of the H7; the functions are declared here and defined by the test, which plays the peripheral and records what the driver wrote.
 */
#ifndef HYDRA_UMC_HOST_STUB_H7_FDCAN_START_HAL_H
#define HYDRA_UMC_HOST_STUB_H7_FDCAN_START_HAL_H

#include <stddef.h>
#include <stdint.h>

typedef enum { HAL_OK = 0, HAL_ERROR = 1, HAL_BUSY = 2, HAL_TIMEOUT = 3 } HAL_StatusTypeDef;

#define DISABLE 0U
#define ENABLE 1U

typedef struct { int instance_id; } FDCAN_GlobalTypeDef;

typedef struct {
    uint32_t FrameFormat, Mode, AutoRetransmission, TransmitPause, ProtocolException;
    uint32_t NominalPrescaler, NominalSyncJumpWidth, NominalTimeSeg1, NominalTimeSeg2;
    uint32_t DataPrescaler, DataSyncJumpWidth, DataTimeSeg1, DataTimeSeg2;
    uint32_t StdFiltersNbr, ExtFiltersNbr, MessageRAMOffset;
    uint32_t RxFifo0ElmtsNbr, RxFifo0ElmtSize, RxFifo1ElmtsNbr, RxBuffersNbr, TxEventsNbr, TxBuffersNbr, TxFifoQueueElmtsNbr, TxFifoQueueMode, TxElmtSize;
} FDCAN_InitTypeDef;

typedef struct {
    FDCAN_GlobalTypeDef *Instance;
    FDCAN_InitTypeDef Init;
} FDCAN_HandleTypeDef;

typedef struct { uint32_t IdType, FilterIndex, FilterType, FilterConfig, FilterID1, FilterID2; } FDCAN_FilterTypeDef;
typedef struct { uint32_t TxErrorCnt, RxErrorCnt, RxErrorPassive, ErrorLogging; } FDCAN_ErrorCountersTypeDef;

#define FDCAN_FRAME_CLASSIC 0x00000000U
#define FDCAN_MODE_NORMAL 0x00000000U
#define FDCAN_DATA_BYTES_8 0x00000004U
#define FDCAN_TX_FIFO_OPERATION 0x00000000U
#define FDCAN_STANDARD_ID 0x00000000U
#define FDCAN_FILTER_MASK 0x00000002U
#define FDCAN_FILTER_TO_RXFIFO0 0x00000001U
#define FDCAN_REJECT 0x00000001U
#define FDCAN_REJECT_REMOTE 0x00000001U

HAL_StatusTypeDef HAL_FDCAN_Init(FDCAN_HandleTypeDef *hfdcan);
HAL_StatusTypeDef HAL_FDCAN_ConfigFilter(FDCAN_HandleTypeDef *hfdcan, FDCAN_FilterTypeDef *filter);
HAL_StatusTypeDef HAL_FDCAN_ConfigGlobalFilter(FDCAN_HandleTypeDef *hfdcan, uint32_t non_matching_std, uint32_t non_matching_ext, uint32_t reject_remote_std, uint32_t reject_remote_ext);
HAL_StatusTypeDef HAL_FDCAN_Start(FDCAN_HandleTypeDef *hfdcan);
HAL_StatusTypeDef HAL_FDCAN_GetErrorCounters(FDCAN_HandleTypeDef *hfdcan, FDCAN_ErrorCountersTypeDef *counters);

#endif /* HYDRA_UMC_HOST_STUB_H7_FDCAN_START_HAL_H */
