// =============================================================================
// HYDRA-UMC Kinematic Brain Bootloader (CM4) - clock config, SPI1 + FDCAN1
// init, main gateway loop, and interrupt handlers
// Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
// GPL-3.0 - see repo root LICENSE
//
// This is the one bootloader in the whole project that ISN'T a simple
// "listen on one bus, run the update state machine" loop - it's a gateway
// sitting between 3 different transports (SPI1 to the CM5, FDCAN1 to STACK
// A, the IPC mailbox to CM7), routing every received SPI1 frame to exactly
// one of them per bootloader_common.h's own SpiOtaFrame_t.target_tier, and
// relaying the answer back. See docs/architecture.md section 3 for why
// this core - not CM7 - is the one that owns SPI1 in the first place
// (README.md section 10 already established the link is CM4's own before
// any of this CAN-OTA design existed).
//
// Pin assignments below match docs/PINOUT_STM32H745_KINEMATIC_BRAIN.TXT
// sections 1-2 exactly - SPI1 (PA4/5/6/7), HYDRA_DATA_READY (PA3), FDCAN1
// (PD0/PD1), CAN1_STBY (PG7).
// =============================================================================
#include "stm32h7xx_hal.h"
#include <string.h>
#include "bootloader_common.h"
#include "bootloader_crypto.h"
#include "bootloader_flash.h"
#include "bootloader_protocol.h"
#include "ipc_mailbox.h"

SPI_HandleTypeDef   hspi1;
FDCAN_HandleTypeDef hfdcan1;
IWDG_HandleTypeDef  hiwdg;

#define HYDRA_DATA_READY_PORT GPIOA
#define HYDRA_DATA_READY_PIN  GPIO_PIN_3

void SystemClock_Config(void) {
    // HSI64 reset default - TODO once a real dual-core clock tree exists,
    // same placeholder as CM7's own bootloader_main.c.
}

static void MX_SPI1_Init(void) {
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_SPI1_CLK_ENABLE();

    GPIO_InitTypeDef gi = {0};
    gi.Pin = GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7;
    gi.Mode = GPIO_MODE_AF_PP;
    gi.Pull = GPIO_NOPULL;
    gi.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gi.Alternate = GPIO_AF5_SPI1;
    HAL_GPIO_Init(GPIOA, &gi);

    GPIO_InitTypeDef gi_dr = {0};
    gi_dr.Pin = HYDRA_DATA_READY_PIN;
    gi_dr.Mode = GPIO_MODE_OUTPUT_PP;
    gi_dr.Pull = GPIO_NOPULL;
    gi_dr.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(HYDRA_DATA_READY_PORT, &gi_dr);
    HAL_GPIO_WritePin(HYDRA_DATA_READY_PORT, HYDRA_DATA_READY_PIN, GPIO_PIN_RESET);

    hspi1.Instance = SPI1;
    hspi1.Init.Mode = SPI_MODE_SLAVE;
    hspi1.Init.Direction = SPI_DIRECTION_2LINES;
    hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
    hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;
    hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;
    hspi1.Init.NSS = SPI_NSS_HARD_INPUT; // PA4 as hardware NSS - the CM5 (SPI master) drives it per-transaction
    hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
    hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
    hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    HAL_SPI_Init(&hspi1);
}

static void MX_FDCAN1_Init(void) {
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOG_CLK_ENABLE();
    __HAL_RCC_FDCAN_CLK_ENABLE();

    GPIO_InitTypeDef gi = {0};
    gi.Pin = GPIO_PIN_0 | GPIO_PIN_1;
    gi.Mode = GPIO_MODE_AF_PP;
    gi.Pull = GPIO_NOPULL;
    gi.Speed = GPIO_SPEED_FREQ_HIGH;
    gi.Alternate = GPIO_AF9_FDCAN1;
    HAL_GPIO_Init(GPIOD, &gi);

    GPIO_InitTypeDef gi_stby = {0};
    gi_stby.Pin = GPIO_PIN_7;
    gi_stby.Mode = GPIO_MODE_OUTPUT_PP;
    gi_stby.Pull = GPIO_NOPULL;
    gi_stby.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOG, &gi_stby);
    HAL_GPIO_WritePin(GPIOG, GPIO_PIN_7, GPIO_PIN_RESET);

    hfdcan1.Instance = FDCAN1;
    hfdcan1.Init.FrameFormat = FDCAN_FRAME_CLASSIC;
    hfdcan1.Init.Mode = FDCAN_MODE_NORMAL;
    hfdcan1.Init.AutoRetransmission = ENABLE;
    hfdcan1.Init.TransmitPause = DISABLE;
    hfdcan1.Init.ProtocolException = DISABLE;
    // Same "1Mbps @ assumed 16 time quanta/bit" reasoning as the G474
    // bootloader's own MX_FDCAN1_Init - re-verify once this chip's real
    // clock tree (currently HSI64 reset default) is designed, since the
    // FDCAN kernel clock source/frequency assumption this prescaler is
    // built on will very likely change alongside it.
    hfdcan1.Init.NominalPrescaler = 1;
    hfdcan1.Init.NominalSyncJumpWidth = 1;
    hfdcan1.Init.NominalTimeSeg1 = 13;
    hfdcan1.Init.NominalTimeSeg2 = 2;
    hfdcan1.Init.DataPrescaler = 1;
    hfdcan1.Init.DataSyncJumpWidth = 1;
    hfdcan1.Init.DataTimeSeg1 = 13;
    hfdcan1.Init.DataTimeSeg2 = 2;
    hfdcan1.Init.MessageRAMOffset = 0;
    hfdcan1.Init.StdFiltersNbr = 1;
    hfdcan1.Init.ExtFiltersNbr = 0;
    hfdcan1.Init.RxFifo0ElmtsNbr = 8;
    hfdcan1.Init.RxFifo0ElmtSize = FDCAN_DATA_BYTES_8;
    hfdcan1.Init.RxFifo1ElmtsNbr = 0;
    hfdcan1.Init.RxBuffersNbr = 0;
    hfdcan1.Init.TxEventsNbr = 0;
    hfdcan1.Init.TxBuffersNbr = 0;
    hfdcan1.Init.TxFifoQueueElmtsNbr = 8;
    hfdcan1.Init.TxFifoQueueMode = FDCAN_TX_FIFO_OPERATION;
    hfdcan1.Init.TxElmtSize = FDCAN_DATA_BYTES_8;
    HAL_FDCAN_Init(&hfdcan1);

    FDCAN_FilterTypeDef sf;
    sf.IdType = FDCAN_STANDARD_ID;
    sf.FilterIndex = 0;
    sf.FilterType = FDCAN_FILTER_MASK;
    sf.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
    sf.FilterID1 = 0x000;
    sf.FilterID2 = 0x000; // wide open, same reasoning as every other FDCAN bootloader in this project - this core sees traffic from ALL 8 STACK A slots, not just one, so a narrow filter would need to be reconfigured per-relay anyway
    HAL_FDCAN_ConfigFilter(&hfdcan1, &sf);
    HAL_FDCAN_ConfigGlobalFilter(&hfdcan1, FDCAN_ACCEPT_IN_RX_FIFO0, FDCAN_ACCEPT_IN_RX_FIFO0, FDCAN_REJECT_REMOTE, FDCAN_REJECT_REMOTE);

    HAL_FDCAN_Start(&hfdcan1);
}

// Converts an SpiOtaFrame_t into the raw 128-byte wire format and back -
// SPI is a raw byte stream, so this bootloader defines the on-wire layout
// as simply the struct's own field order (target_tier, target_slot,
// frame_type, dlc, payload[8], then zero-padding to 128 bytes) rather than
// a separately-specified wire format - there's only ever one reader/writer
// pair (this bootloader and whatever runs on the CM5), so no cross-
// language serialization concern applies the way it might for the FDCAN
// side (which URTC's own tooling also has to speak).
static void FrameToWire(const SpiOtaFrame_t *f, uint8_t wire[SPI_FRAME_SIZE]) {
    memset(wire, 0xFF, SPI_FRAME_SIZE);
    wire[0] = f->target_tier;
    wire[1] = f->target_slot;
    wire[2] = f->frame_type;
    wire[3] = f->dlc;
    memcpy(&wire[4], f->payload, 8);
}

static void WireToFrame(const uint8_t wire[SPI_FRAME_SIZE], SpiOtaFrame_t *f) {
    memset(f, 0, sizeof(*f));
    f->target_tier = wire[0];
    f->target_slot = wire[1];
    f->frame_type = wire[2];
    f->dlc = wire[3];
    memcpy(f->payload, &wire[4], 8);
}

static uint32_t last_heartbeat_tick = 0 - 1000;

int main(void) {
    HAL_Init();
    SystemClock_Config();
    __HAL_RCC_HSEM_CLK_ENABLE();

    MX_SPI1_Init();
    MX_FDCAN1_Init();

    hiwdg.Instance = IWDG2; // CM4 owns IWDG2 (CM7 owns IWDG1) - independent per-core watchdogs, docs/architecture.md section 2
    hiwdg.Init.Prescaler = IWDG_PRESCALER_32;
    hiwdg.Init.Reload = 4095; // generous placeholder, same TODO as CM7's own bootloader_main.c
    HAL_IWDG_Init(&hiwdg);

    uint8_t app_valid = ApplicationIsValid();

    uint8_t tx_wire[SPI_FRAME_SIZE];
    uint8_t rx_wire[SPI_FRAME_SIZE];
    SpiOtaFrame_t idle_frame = {0};
    idle_frame.frame_type = OFS_STATUS;
    idle_frame.dlc = 1;
    idle_frame.payload[0] = STATUS_LISTENING;
    FrameToWire(&idle_frame, tx_wire);

    uint32_t listen_start = HAL_GetTick();
    uint8_t enter_update_mode = 0;

    // Listening window: same ~600ms convention as every other bootloader,
    // except here "listening" means actively servicing SPI1 transactions
    // (the CM5 might not even attempt one during this window in normal
    // operation - HAL_SPI_TransmitReceive's own short timeout below just
    // means this loop moves on rather than blocking the whole window on a
    // single transaction attempt).
    while ((HAL_GetTick() - listen_start) < 600) {
        HAL_IWDG_Refresh(&hiwdg);
        HAL_GPIO_WritePin(HYDRA_DATA_READY_PORT, HYDRA_DATA_READY_PIN, GPIO_PIN_SET);
        if (HAL_SPI_TransmitReceive(&hspi1, tx_wire, rx_wire, SPI_FRAME_SIZE, 20) == HAL_OK) {
            SpiOtaFrame_t in;
            WireToFrame(rx_wire, &in);
            if (in.target_tier == SPI_TARGET_SELF && in.frame_type == OFS_START_UPDATE && in.dlc == 8) {
                enter_update_mode = 1;
                HandleStartUpdate(in.payload);
                break;
            }
            // Anything else during the listening window (version queries,
            // relayed traffic) is handled the same way the main loop below
            // does - see that loop's own comments, not duplicated here.
        }
        HAL_GPIO_WritePin(HYDRA_DATA_READY_PORT, HYDRA_DATA_READY_PIN, GPIO_PIN_RESET);

        if (HAL_GetTick() - last_heartbeat_tick >= 1000) {
            last_heartbeat_tick = HAL_GetTick();
            CAN_SendHeartbeat(STATUS_LISTENING, 0xFF);
        }
        SpiOtaFrame_t pending;
        if (Protocol_TakePendingResponse(&pending)) FrameToWire(&pending, tx_wire);
    }

    if (!enter_update_mode && app_valid) {
        JumpToApplication();
    }

    CAN_SendStatus(STATUS_LISTENING);
    while (1) {
        HAL_IWDG_Refresh(&hiwdg);
        HAL_GPIO_WritePin(HYDRA_DATA_READY_PORT, HYDRA_DATA_READY_PIN, GPIO_PIN_SET);
        if (HAL_SPI_TransmitReceive(&hspi1, tx_wire, rx_wire, SPI_FRAME_SIZE, 20) == HAL_OK) {
            SpiOtaFrame_t in, out;
            WireToFrame(rx_wire, &in);

            if (in.target_tier == SPI_TARGET_STACKA) {
                Relay_ToStackA(in.target_slot, &in, &out);
                FrameToWire(&out, tx_wire);
            } else if (in.target_tier == SPI_TARGET_CM7) {
                Relay_ToCM7(&in, &out);
                FrameToWire(&out, tx_wire);
            } else { // SPI_TARGET_SELF
                if (in.frame_type == OFS_START_UPDATE && in.dlc == 8) {
                    HandleStartUpdate(in.payload);
                } else if (in.frame_type == OFS_HMAC_CHUNK && in.dlc == 8) {
                    HandleHmacChunk(in.payload);
                } else if (in.frame_type == OFS_DATA) {
                    HandleData(in.payload, in.dlc);
                } else if (in.frame_type == OFS_END_UPDATE && in.dlc == 8) {
                    HandleEndUpdate(in.payload);
                } else if (in.frame_type == OFS_QUERY_VERSION) {
                    HandleVersionQuery();
                } else if (in.frame_type == OFS_QUERY_ERROR_COUNTERS) {
                    HandleErrorCounterQuery();
                } else if (in.frame_type == OFS_AUTHORIZE_DOWNGRADE && in.dlc == 4) {
                    HandleAuthorizeDowngrade(in.payload);
                } else if (in.frame_type == OFS_BACKUP_READ_REQUEST) {
                    HandleReadbackStart();
                }
            }
        }
        HAL_GPIO_WritePin(HYDRA_DATA_READY_PORT, HYDRA_DATA_READY_PIN, GPIO_PIN_RESET);

        SpiOtaFrame_t pending;
        if (Protocol_TakePendingResponse(&pending)) {
            FrameToWire(&pending, tx_wire);
            // The self-update flow's own success frame (STATUS_VERIFY_OK)
            // is stashed by HandleEndUpdate but NOT immediately followed
            // by a reset the way every other bootloader's own HandleEndUpdate
            // does - see that function's own comment. This is where the
            // deferred reset actually happens: once the success status has
            // been staged to go out on the (already-completed, in this
            // same loop iteration) SPI1 transaction above... actually one
            // transaction too early - see the note below.
            if (pending.target_tier == SPI_TARGET_SELF && pending.frame_type == OFS_STATUS &&
                pending.dlc == 1 && pending.payload[0] == STATUS_VERIFY_OK) {
                // tx_wire now holds the success frame - give it exactly one
                // more SPI1 transaction to actually reach the CM5 before
                // resetting, same "let the status frame get out first"
                // reasoning every other bootloader's own HAL_Delay()-then-
                // reset already applies, just expressed as one more
                // transaction instead of a fixed delay (SPI only moves
                // data when the master clocks it, a delay alone wouldn't
                // guarantee this frame was actually sent).
                HAL_IWDG_Refresh(&hiwdg);
                HAL_GPIO_WritePin(HYDRA_DATA_READY_PORT, HYDRA_DATA_READY_PIN, GPIO_PIN_SET);
                HAL_SPI_TransmitReceive(&hspi1, tx_wire, rx_wire, SPI_FRAME_SIZE, 100);
                HAL_GPIO_WritePin(HYDRA_DATA_READY_PORT, HYDRA_DATA_READY_PIN, GPIO_PIN_RESET);
                HAL_Delay(100);
                NVIC_SystemReset();
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

void HardFault_Handler(void) {
    GPIOA->MODER = 0xEBFFFFFF;
    GPIOB->MODER = 0xFFFFFFFF;
    while (1) { }
}
void MemManage_Handler(void) { while (1) { } }
void BusFault_Handler(void) { while (1) { } }
void UsageFault_Handler(void) { while (1) { } }
void NMI_Handler(void) { }
void SVC_Handler(void) { }
void DebugMon_Handler(void) { }
void PendSV_Handler(void) { }
void SysTick_Handler(void) { HAL_IncTick(); }
