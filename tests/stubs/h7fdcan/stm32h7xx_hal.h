/* =============================================================================
 * HYDRA-UMC Firmware - Host test stand-in for the STM32H7 HAL header (FDCAN)
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Just enough of the vendor header for the FDCAN1 binding of the gateway (src/mcu_stm32h757/CM7/GatewayFdcan1.c) to compile with the host's plain gcc. The functions are declared
 * here and defined by the test, which plays the peripheral and records what the driver did.
 */
#ifndef HYDRA_UMC_HOST_STUB_H7_FDCAN_HAL_H
#define HYDRA_UMC_HOST_STUB_H7_FDCAN_HAL_H

#include <stddef.h>
#include <stdint.h>

typedef enum { HAL_OK = 0, HAL_ERROR = 1, HAL_BUSY = 2, HAL_TIMEOUT = 3 } HAL_StatusTypeDef;

typedef struct { int instance_id; } FDCAN_GlobalTypeDef;
typedef struct { FDCAN_GlobalTypeDef *Instance; } FDCAN_HandleTypeDef;

typedef struct {
    uint32_t Identifier, IdType, TxFrameType, DataLength, ErrorStateIndicator, BitRateSwitch, FDFormat, TxEventFifoControl, MessageMarker;
} FDCAN_TxHeaderTypeDef;

typedef struct {
    uint32_t Identifier, IdType, RxFrameType, DataLength, ErrorStateIndicator, BitRateSwitch, FDFormat, RxTimestamp, FilterIndex, IsFilterMatchingFrame;
} FDCAN_RxHeaderTypeDef;

#define FDCAN_STANDARD_ID 0x00000000U
#define FDCAN_EXTENDED_ID 0x40000000U
#define FDCAN_DATA_FRAME 0x00000000U
#define FDCAN_REMOTE_FRAME 0x20000000U
#define FDCAN_ESI_ACTIVE 0x00000000U
#define FDCAN_BRS_OFF 0x00000000U
#define FDCAN_CLASSIC_CAN 0x00000000U
#define FDCAN_NO_TX_EVENTS 0x00000000U
#define FDCAN_RX_FIFO0 0x00000040U

uint32_t HAL_FDCAN_GetTxFifoFreeLevel(FDCAN_HandleTypeDef *hfdcan);
HAL_StatusTypeDef HAL_FDCAN_AddMessageToTxFifoQ(FDCAN_HandleTypeDef *hfdcan, FDCAN_TxHeaderTypeDef *header, uint8_t *data);
uint32_t HAL_FDCAN_GetRxFifoFillLevel(FDCAN_HandleTypeDef *hfdcan, uint32_t fifo);
HAL_StatusTypeDef HAL_FDCAN_GetRxMessage(FDCAN_HandleTypeDef *hfdcan, uint32_t fifo, FDCAN_RxHeaderTypeDef *header, uint8_t *data);
uint32_t HAL_GetTick(void);

#endif /* HYDRA_UMC_HOST_STUB_H7_FDCAN_HAL_H */
