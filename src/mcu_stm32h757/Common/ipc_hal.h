// =============================================================================
// HYDRA-UMC Kinematic Brain - the callbacks of ipc_link.h over the HAL (hardware semaphores, data barrier, tick, watchdog)
// Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
// GPL-3.0-or-later - see repo root LICENSE
//
// The only file of the mailbox that touches the HAL; the operations themselves are in ipc_link.h and are run by the host tests. It is included by the bootloader of each core, which
// defines the watchdog handle `hiwdg`. HAL_HSEM_FastTake returns HAL_OK only for the core that took the semaphore, and HAL_HSEM_Release (a write with the core identifier of the caller)
// frees only a semaphore that same core took: ipc_link.h never releases one it did not take. The context pointer of the callbacks is not used.
// Nothing here has run on a board.
// =============================================================================
#ifndef HYDRA_IPC_HAL_H
#define HYDRA_IPC_HAL_H

#include "stm32h7xx_hal.h"
#include "ipc_link.h"

extern IWDG_HandleTypeDef hiwdg;

static inline uint32_t IpcHal_Tick(void *ctx) { (void)ctx; return HAL_GetTick(); }
static inline void IpcHal_Refresh(void *ctx) { (void)ctx; HAL_IWDG_Refresh(&hiwdg); }
static inline int IpcHal_Taken(void *ctx, uint32_t channel) { (void)ctx; return HAL_HSEM_IsSemTaken(channel) ? 1 : 0; }
static inline int IpcHal_Take(void *ctx, uint32_t channel) { (void)ctx; return HAL_HSEM_FastTake(channel) == HAL_OK ? 1 : 0; }
static inline void IpcHal_Release(void *ctx, uint32_t channel) { (void)ctx; HAL_HSEM_Release(channel, 0); }
// SRAM4 writes are Normal (bufferable) memory and the semaphore take is a peripheral (Device) write: without this barrier the take could reach the other core before the frame has
// landed in SRAM4 (the same "flush shared data before signaling" the dual-core application notes of the family require).
static inline void IpcHal_Barrier(void *ctx) { (void)ctx; __DSB(); }

static const IpcOps IPC_HAL_OPS = {IpcHal_Tick, IpcHal_Refresh, IpcHal_Taken, IpcHal_Take, IpcHal_Release, IpcHal_Barrier};

#endif // HYDRA_IPC_HAL_H
