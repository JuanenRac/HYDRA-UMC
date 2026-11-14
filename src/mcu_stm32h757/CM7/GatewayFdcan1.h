/* =============================================================================
 * HYDRA-UMC - src/mcu_stm32h757/CM7/GatewayFdcan1.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * The binding of the `stack_a` tier of the gateway of the CM7 (Common/gateway_stack_a.h) to FDCAN1: it gives the operations of that header the HAL calls that queue a Classic CAN
 * frame, take one from RX FIFO0 and read the tick. OFF by default: with HYDRA_GATEWAY_FDCAN1 0 (as it ships) GatewayFdcan1.c compiles to an empty unit. It does not start FDCAN1
 * (bit timing, filters and the clock stay with the set-up of the bus, which has to run before): the caller hands it a peripheral that is already started and the time-out of the wait
 * from config/gateway.cfg. Not run on a board: the exchange with a controller board on the stack bus remains.
 * ========================================================================== */
#ifndef HYDRA_UMC_GATEWAY_FDCAN1_H
#define HYDRA_UMC_GATEWAY_FDCAN1_H

#ifndef HYDRA_GATEWAY_FDCAN1
#define HYDRA_GATEWAY_FDCAN1 0
#endif

#if HYDRA_GATEWAY_FDCAN1

#include "stm32h7xx_hal.h"
#include "gateway_stack_a.h"

/* Binds the stack tier to an FDCAN1 that is already started. `kick` (may be NULL) is called on every turn of the wait. Returns the object to hand to the gateway as the context of
 * GatewayStackA_Exchange, or NULL when the peripheral is missing or the time-out is outside 1..1000 ms. */
GatewayStackA_t *GatewayFdcan1_Bind(FDCAN_HandleTypeDef *hfdcan, uint32_t timeout_ms, void (*kick)(void));

#endif /* HYDRA_GATEWAY_FDCAN1 */

#endif /* HYDRA_UMC_GATEWAY_FDCAN1_H */
