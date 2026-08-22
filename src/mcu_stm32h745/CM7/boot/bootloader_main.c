// =============================================================================
// HYDRA-UMC Kinematic Brain Bootloader (CM7) - mailbox polling main loop
// and interrupt handlers
// Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
// GPL-3.0 - see repo root LICENSE
//
// Ported from URTC's own proven bootloader_main.c via HYDRA-UMC's own G474
// port - same listening-window/update/heartbeat/timeout structure. Real
// difference: this core has no bus of its own to initialize or poll (no
// MX_xxx_Init() here) - it polls the CM7<->CM4 IPC mailbox's own
// IPC_HSEM_CMD_ID instead of a peripheral RX FIFO. Every command this
// bootloader ever processes was relayed here by CM4's own bootloader
// (../../CM4/boot/bootloader_main.c) after arriving over SPI1 from the
// CM5 - see docs/architecture.md section 3.
//
// D1/D2 domain clock-tree bring-up (CM7 and CM4 both individually reset
// into their own default HSI64 state on this dual-core chip - see docs/
// COMPILE_STM32H745.TXT section 6) is still TODO exactly as flagged
// throughout this project; SystemClock_Config() below is a placeholder for
// where that goes, same convention as every other bootloader here.
// =============================================================================
#include "stm32h7xx_hal.h"
#include <string.h>
#include "bootloader_common.h"
#include "bootloader_crypto.h"
#include "bootloader_flash.h"
#include "bootloader_protocol.h"
#include "ipc_mailbox.h"

IWDG_HandleTypeDef hiwdg;

void SystemClock_Config(void) {
    // HSI64 reset default - TODO once a real dual-core clock tree exists,
    // see this file's own header comment.
}

static uint32_t last_heartbeat_tick = 0 - 1000;

static void DispatchCommand(uint8_t ofs, uint8_t *data, uint8_t dlc) {
    if (ofs == OFS_QUERY_VERSION) {
        HandleVersionQuery();
    } else if (ofs == OFS_QUERY_ERROR_COUNTERS) {
        HandleErrorCounterQuery();
    } else if (ofs == OFS_BACKUP_READ_REQUEST) {
        HandleReadbackStart();
    } else if (ofs == OFS_HMAC_CHUNK && dlc == 8) {
        HandleHmacChunk(data);
    } else if (ofs == OFS_DATA) {
        HandleData(data, dlc);
    } else if (ofs == OFS_END_UPDATE && dlc == 8) {
        HandleEndUpdate(data);
    } else if (ofs == OFS_AUTHORIZE_DOWNGRADE && dlc == 4) {
        HandleAuthorizeDowngrade(data);
    }
}

int main(void) {
    // IWDG armed as the very first action in main(), before HAL_Init() -
    // if HAL_Init()/SystemClock_Config() themselves ever got stuck, the
    // watchdog is already ticking and will still recover the board
    // instead of leaving the chip wedged with no independent watchdog
    // running yet. Safe this early: IWDG1 runs off its own always-on LSI
    // domain (no RCC/clock-tree dependency), and HAL_IWDG_Init()'s PVU/RVU
    // register-update-flag wait polls hardware status bits directly, not
    // HAL_GetTick() - so it doesn't need HAL_Init()'s SysTick config to
    // have run first either. Same IWDG shape as every other bootloader in
    // this project - see this file's own header comment on why the exact
    // prescaler/reload is still a placeholder.
    hiwdg.Instance = IWDG1; // CM7 owns IWDG1, CM4 owns IWDG2 - two independent watchdogs, matching this chip's own dual-core independence (docs/architecture.md section 2)
    hiwdg.Init.Prescaler = IWDG_PRESCALER_32;
    hiwdg.Init.Reload = 4095; // generous placeholder - TODO tune against real 128KB sector-erase timing once measured on real hardware, see bootloader_flash.c's own header
    HAL_IWDG_Init(&hiwdg);

    HAL_Init();
    SystemClock_Config();
    __HAL_RCC_HSEM_CLK_ENABLE();

    uint8_t app_valid = ApplicationIsValid();

    // Listening window: same ~600ms as every other bootloader, except here
    // it's "wait for CM4 to relay a command" instead of "wait for a bus
    // frame" - CM4's own bootloader only starts relaying once IT has
    // decided this core needs updating (its own SPI1 dispatch logic), so
    // in the common case (no update requested) this window elapses with
    // nothing arriving, same as every other tier's own idle listening
    // window elapsing with no traffic.
    uint32_t listen_start = HAL_GetTick();
    uint8_t enter_update_mode = 0;

    while ((HAL_GetTick() - listen_start) < 600) {
        HAL_IWDG_Refresh(&hiwdg);
        if (HAL_HSEM_IsSemTaken(IPC_HSEM_CMD_ID)) {
            uint8_t ofs = IPC_MAILBOX->cmd.frame_type;
            uint8_t dlc = IPC_MAILBOX->cmd.dlc;
            uint8_t data[8];
            memcpy(data, (const void*)IPC_MAILBOX->cmd.payload, 8);
            HAL_HSEM_Release(IPC_HSEM_CMD_ID, 0);

            if (ofs == OFS_START_UPDATE && dlc == 8) {
                enter_update_mode = 1;
                HandleStartUpdate(data);
                break;
            } else {
                DispatchCommand(ofs, data, dlc);
            }
        }
        if (HAL_GetTick() - last_heartbeat_tick >= 1000) {
            last_heartbeat_tick = HAL_GetTick();
            CAN_SendHeartbeat(STATUS_LISTENING, 0xFF);
        }
    }

    if (!enter_update_mode && app_valid) {
        JumpToApplication();
    }

    CAN_SendStatus(STATUS_LISTENING);
    while (1) {
        HAL_IWDG_Refresh(&hiwdg);
        if (HAL_HSEM_IsSemTaken(IPC_HSEM_CMD_ID)) {
            uint8_t ofs = IPC_MAILBOX->cmd.frame_type;
            uint8_t dlc = IPC_MAILBOX->cmd.dlc;
            uint8_t data[8];
            memcpy(data, (const void*)IPC_MAILBOX->cmd.payload, 8);
            HAL_HSEM_Release(IPC_HSEM_CMD_ID, 0);

            if (ofs == OFS_START_UPDATE && dlc == 8) {
                HandleStartUpdate(data);
            } else {
                DispatchCommand(ofs, data, dlc);
            }
        }

        if (HAL_GetTick() - last_heartbeat_tick >= 1000) {
            last_heartbeat_tick = HAL_GetTick();
            uint8_t pct = 0xFF;
            uint8_t hb_status = STATUS_LISTENING;
            if (update_in_progress && !update_failed && update_total_size > 0) {
                pct = (uint8_t)(((uint64_t)update_bytes_received * 100) / update_total_size);
                hb_status = STATUS_RECEIVING;
            }
            CAN_SendHeartbeat(hb_status, pct);
        }

        if (update_in_progress && !update_failed &&
            (HAL_GetTick() - update_last_activity_tick > 10000)) {
            update_in_progress = 0;
            update_failed = 0;
            CAN_SendStatus(STATUS_LISTENING);
        }
    }
}

void HardFault_Handler(void) { while (1) { } }
void MemManage_Handler(void) { while (1) { } }
void BusFault_Handler(void) { while (1) { } }
void UsageFault_Handler(void) { while (1) { } }
void NMI_Handler(void) { }
void SVC_Handler(void) { }
void DebugMon_Handler(void) { }
void PendSV_Handler(void) { }
void SysTick_Handler(void) { HAL_IncTick(); }
