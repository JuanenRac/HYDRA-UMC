/* =============================================================================
 * HYDRA-UMC - src/mcu_stm32h757/CM7/GatewayFdcan1.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * FDCAN1 operations of the stack tier of the gateway; see GatewayFdcan1.h and Common/gateway_stack_a.h. The frames are Classic CAN with a standard id, as on the CM4 of the H745
 * board (CM4/boot/bootloader_protocol.c, Relay_ToStackA); a received frame that is not a standard data frame is taken out of the FIFO and ignored.
 * ========================================================================== */
#include "GatewayFdcan1.h"

#if HYDRA_GATEWAY_FDCAN1

static FDCAN_HandleTypeDef *g_fdcan;
static void (*g_kick)(void);
static GatewayStackA_t g_stack;

static int GatewayFdcan1_Tx(void *ctx, uint32_t id, const uint8_t *data, uint8_t dlc)
{
    (void)ctx;
    if (HAL_FDCAN_GetTxFifoFreeLevel(g_fdcan) == 0u) return 0;
    FDCAN_TxHeaderTypeDef header;
    header.Identifier = id;
    header.IdType = FDCAN_STANDARD_ID;
    header.TxFrameType = FDCAN_DATA_FRAME;
    header.DataLength = dlc > 8u ? 8u : dlc; /* the same plain code as DlcToFdcanDataLength of the CM4 */
    header.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
    header.BitRateSwitch = FDCAN_BRS_OFF;
    header.FDFormat = FDCAN_CLASSIC_CAN;
    header.TxEventFifoControl = FDCAN_NO_TX_EVENTS;
    header.MessageMarker = 0u;
    uint8_t copy[8];
    for (unsigned i = 0; i < 8u; i++) copy[i] = data[i];
    return HAL_FDCAN_AddMessageToTxFifoQ(g_fdcan, &header, copy) == HAL_OK;
}

static int GatewayFdcan1_Rx(void *ctx, uint32_t *id, uint8_t *data, uint8_t *dlc)
{
    (void)ctx;
    if (HAL_FDCAN_GetRxFifoFillLevel(g_fdcan, FDCAN_RX_FIFO0) == 0u) return 0;
    FDCAN_RxHeaderTypeDef header;
    if (HAL_FDCAN_GetRxMessage(g_fdcan, FDCAN_RX_FIFO0, &header, data) != HAL_OK) return 0;
    if (header.RxFrameType != FDCAN_DATA_FRAME || header.IdType != FDCAN_STANDARD_ID) return 0;
    *id = header.Identifier;
    *dlc = header.DataLength > 8u ? 8u : (uint8_t)header.DataLength; /* already the plain 4-bit code */
    return 1;
}

static uint32_t GatewayFdcan1_Now(void *ctx)
{
    (void)ctx;
    return HAL_GetTick();
}

static void GatewayFdcan1_Kick(void *ctx)
{
    (void)ctx;
    if (g_kick != NULL) g_kick();
}

GatewayStackA_t *GatewayFdcan1_Bind(FDCAN_HandleTypeDef *hfdcan, uint32_t timeout_ms, void (*kick)(void))
{
    static const GatewayCanOps ops = {GatewayFdcan1_Tx, GatewayFdcan1_Rx, GatewayFdcan1_Now, GatewayFdcan1_Kick};
    if (hfdcan == NULL) return NULL;
    if (!GatewayStackA_Init(&g_stack, &ops, NULL, timeout_ms)) return NULL;
    g_fdcan = hfdcan;
    g_kick = kick;
    return &g_stack;
}

#endif /* HYDRA_GATEWAY_FDCAN1 */
