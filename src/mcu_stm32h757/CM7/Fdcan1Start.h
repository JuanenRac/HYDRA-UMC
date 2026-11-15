/* =============================================================================
 * HYDRA-UMC - src/mcu_stm32h757/CM7/Fdcan1Start.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * The start-up of FDCAN1 on the CM7 for the stack tier of the gateway (CM7/GatewayFdcan1.c binds the tier to a peripheral that is already started; this unit starts it). It writes
 * the plan of Common/fdcan_plan.h into the peripheral in the order of the CM4 of the H745 board (CM4/boot/bootloader_main.c, MX_FDCAN1_Init): the bit timing and the FIFOs, the
 * initialisation, the acceptance filters, the global filter that rejects whatever no filter takes (and the remote frames), and only then the start; a filter that was refused stops
 * the start. OFF by default: with HYDRA_FDCAN1_START 0 (as it ships) Fdcan1Start.c compiles to an empty unit. The clock of the peripheral (the kernel clock from PLL2_Q is in
 * Common/clock_config.h), its pins, the transceiver standby line and the interrupt are the board's set-up and stay outside; nothing here has run on a board.
 * ========================================================================== */
#ifndef HYDRA_UMC_FDCAN1_START_H
#define HYDRA_UMC_FDCAN1_START_H

#ifndef HYDRA_FDCAN1_START
#define HYDRA_FDCAN1_START 0
#endif

#if HYDRA_FDCAN1_START

#include <stdint.h>

#include "stm32h7xx_hal.h"
#include "fdcan_plan.h"

#define FDS_OK 0u
#define FDS_FAULT_ARG 0x01u    /* no handle, no instance, no plan, or a plan with a number of filters that is not 1 or 2 */
#define FDS_FAULT_INIT 0x02u   /* HAL_FDCAN_Init refused the numbers */
#define FDS_FAULT_FILTER 0x04u /* a filter or the global filter was refused: the peripheral is not started */
#define FDS_FAULT_START 0x08u  /* HAL_FDCAN_Start failed */

/* Starts `hfdcan` (its Instance already set to FDCAN1, or to FDCAN2 with a plan placed after the one of FDCAN1: `plan->ram_offset` is its place in the shared message RAM) with `plan`. Returns FDS_OK or the faults found, one bit each. */
uint32_t Fdcan1Start_Run(FDCAN_HandleTypeDef *hfdcan, const FdcpPlan *plan);

/* The counters of the bus, as the callback `error_counters` of Common/gateway_self.h takes them (`ctx` is the FDCAN_HandleTypeDef). Returns 1 when the HAL read them. */
int Fdcan1Start_ErrorCounters(void *ctx, uint8_t *tec, uint8_t *rec);

#endif /* HYDRA_FDCAN1_START */

#endif /* HYDRA_UMC_FDCAN1_START_H */
