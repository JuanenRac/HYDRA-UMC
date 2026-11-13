// =============================================================================
// HYDRA-UMC Kinematic Brain Bootloader (CM4) - clock config, SPI1 + FDCAN1
// init, main gateway loop, and interrupt handlers
// Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
// GPL-3.0-or-later - see repo root LICENSE
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
// Pin assignments below match docs/PINOUT_STM32H745_KINEMATIC_BRAIN.md
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
#include "ipc_hal.h"
#include "platform_profile.h" /* the bit timing of the stack bus comes from the platform profile, not from numbers here */
#include "boot_decision.h"
#include "spi_frame_crc.h"
#include "spi_system.h"   // telemetry and probe commands of the SPI link (off by default)

SPI_HandleTypeDef   hspi1;
FDCAN_HandleTypeDef hfdcan1;
IWDG_HandleTypeDef  hiwdg;

// Failures of the HAL set-up calls, one bit each. A bootloader that cannot bring up its link must still be able to start a valid application,
// so the failures are recorded here and decided on in main() instead of being ignored.
#define BOOT_FAULT_CLOCK        0x01u
#define BOOT_FAULT_CAN_INIT     0x02u
#define BOOT_FAULT_CAN_FILTER   0x04u
#define BOOT_FAULT_CAN_START    0x08u
#define BOOT_FAULT_SPI_INIT     0x10u
#define BOOT_FAULT_LINK_MASK    (BOOT_FAULT_CAN_INIT | BOOT_FAULT_CAN_FILTER | BOOT_FAULT_CAN_START | BOOT_FAULT_SPI_INIT)
volatile uint32_t boot_hal_fault = 0;
#define BOOT_HAL_NOTE(call, bit) do { if ((call) != HAL_OK) boot_hal_fault |= (bit); } while (0)

// Without an armed watchdog nothing recovers a stuck board: stop in a state with the interrupts off instead of running unguarded.
static void Boot_WatchdogFailed(void) {
    __disable_irq();
    for (;;) { }
}

// STACK A, seen from the kinematic brain: the windows of all 8 slots (0x600 .. 0x7FF, mask 0x600) and the emergency stop (0x001).
#define BOOT_STACKA_FILTER_ID   0x600u
#define BOOT_STACKA_FILTER_MASK 0x600u
#define BOOT_ESTOP_CAN_ID       0x001u

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
    BOOT_HAL_NOTE(HAL_SPI_Init(&hspi1), BOOT_FAULT_SPI_INIT);
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
    hfdcan1.Init.NominalPrescaler = HYDRA_STACK_BUS_PRESCALER;
    hfdcan1.Init.NominalSyncJumpWidth = HYDRA_STACK_BUS_SYNC_JUMP_WIDTH;
    hfdcan1.Init.NominalTimeSeg1 = HYDRA_STACK_BUS_TIME_SEG1;
    hfdcan1.Init.NominalTimeSeg2 = HYDRA_STACK_BUS_TIME_SEG2;
    hfdcan1.Init.DataPrescaler = HYDRA_STACK_BUS_PRESCALER;
    hfdcan1.Init.DataSyncJumpWidth = HYDRA_STACK_BUS_SYNC_JUMP_WIDTH;
    hfdcan1.Init.DataTimeSeg1 = HYDRA_STACK_BUS_TIME_SEG1;
    hfdcan1.Init.DataTimeSeg2 = HYDRA_STACK_BUS_TIME_SEG2;
    hfdcan1.Init.MessageRAMOffset = 0;
    hfdcan1.Init.StdFiltersNbr = 2;
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
    BOOT_HAL_NOTE(HAL_FDCAN_Init(&hfdcan1), BOOT_FAULT_CAN_INIT);
    if (boot_hal_fault & BOOT_FAULT_CAN_INIT) return; // the peripheral is not usable: no filter, no start

    FDCAN_FilterTypeDef sf;
    sf.IdType = FDCAN_STANDARD_ID;
    sf.FilterIndex = 0;
    sf.FilterType = FDCAN_FILTER_MASK;
    sf.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
    sf.FilterID1 = BOOT_STACKA_FILTER_ID; // this core sees the traffic of all 8 STACK A slots, so the window is the whole 0x600..0x7FF range
    sf.FilterID2 = BOOT_STACKA_FILTER_MASK;
    BOOT_HAL_NOTE(HAL_FDCAN_ConfigFilter(&hfdcan1, &sf), BOOT_FAULT_CAN_FILTER);
    sf.FilterIndex = 1;
    sf.FilterID1 = BOOT_ESTOP_CAN_ID;
    sf.FilterID2 = 0x7FF; // exact match
    BOOT_HAL_NOTE(HAL_FDCAN_ConfigFilter(&hfdcan1, &sf), BOOT_FAULT_CAN_FILTER);
    BOOT_HAL_NOTE(HAL_FDCAN_ConfigGlobalFilter(&hfdcan1, FDCAN_REJECT, FDCAN_REJECT, FDCAN_REJECT_REMOTE, FDCAN_REJECT_REMOTE), BOOT_FAULT_CAN_FILTER);
    if (boot_hal_fault & BOOT_FAULT_CAN_FILTER) return; // never start with a filter that is not the one that was asked for

    BOOT_HAL_NOTE(HAL_FDCAN_Start(&hfdcan1), BOOT_FAULT_CAN_START);
}

// Converts an SpiOtaFrame_t into the raw 128-byte wire format and back -
// SPI is a raw byte stream, so this bootloader defines the on-wire layout
// as simply the struct's own field order (target_tier, target_slot,
// frame_type, dlc, payload[8], then zero-padding to 128 bytes) rather than
// a separately-specified wire format - there's only ever one reader/writer
// pair (this bootloader and whatever runs on the CM5), so no cross-
// language serialization concern applies the way it might for the FDCAN
// side (which URTC's own tooling also has to speak).
// Integrity of the link (spi_frame_crc.h): every frame this core sends is sealed with a sequence number and a CRC16; a received frame whose seal
// does not match is dropped, a repeated sequence is a retry that is not run twice, and a frame without the seal (the older kind) is still
// accepted unless the build sets HYDRA_SPI_REQUIRE_CRC to 1. The counters are read with a debugger or reported by a later status frame.
#ifndef HYDRA_SPI_REQUIRE_CRC
#define HYDRA_SPI_REQUIRE_CRC 0
#endif
static uint8_t  spi_tx_seq;
static uint8_t  spi_rx_have_last;
static uint8_t  spi_rx_last;
volatile uint32_t spi_crc_errors;   // frames dropped for a wrong CRC
volatile uint32_t spi_retries;      // frames received twice with the same sequence
volatile uint32_t spi_seq_gaps;     // sequence jumps (frames lost on the way)
volatile uint32_t spi_unsealed;     // frames of the older kind without the seal

static void FrameToWire(const SpiOtaFrame_t *f, uint8_t wire[SPI_FRAME_SIZE]) {
    memset(wire, 0xFF, SPI_FRAME_SIZE);
    wire[0] = f->target_tier;
    wire[1] = f->target_slot;
    wire[2] = f->frame_type;
    wire[3] = f->dlc;
    memcpy(&wire[4], f->payload, 8);
    SpiWire_Seal(wire, spi_tx_seq++);
}

static void WireToFrame(const uint8_t wire[SPI_FRAME_SIZE], SpiOtaFrame_t *f) {
    memset(f, 0, sizeof(*f));
    f->target_tier = wire[0];
    f->target_slot = wire[1];
    f->frame_type = wire[2];
    f->dlc = wire[3];
    memcpy(f->payload, &wire[4], 8);
}

// 1 when the received frame is to be run (and `f` is filled in), 0 when it was damaged, was a retry or is refused for lacking the seal.
static uint8_t Spi_AcceptFrame(const uint8_t wire[SPI_FRAME_SIZE], SpiOtaFrame_t *f) {
    uint8_t seq = 0;
    switch (SpiWire_Judge(wire, spi_rx_have_last, spi_rx_last, &seq)) {
        case SPI_ACCEPT_BAD_CRC:
            spi_crc_errors++;
            return 0;
        case SPI_ACCEPT_DUPLICATE:
            spi_retries++;
            return 0;
        case SPI_ACCEPT_UNSEALED:
            spi_unsealed++;
            if (HYDRA_SPI_REQUIRE_CRC) return 0;
            break;
        default:
            if (spi_rx_have_last && (uint8_t)(seq - spi_rx_last) != 1u) spi_seq_gaps++;
            spi_rx_have_last = 1;
            spi_rx_last = seq;
            break;
    }
    WireToFrame(wire, f);
    return 1;
}

// Dispatches one received SPI1 frame to the STACK A relay, the CM7 relay,
// or this core's own protocol handlers, writing any immediate reply into
// tx_wire (STACKA/CM7 replies come back synchronously from the relay
// call; SELF-tier replies are staged separately via Protocol_TakePending
// Response, same as before this function existed). Shared between the
// listening window below and the main loop further down so version
// queries / relayed traffic / etc. are handled identically in both -
// previously only the main loop dispatched anything besides
// OFS_START_UPDATE, so the CM5 querying anything else in the first
// ~600ms after reset was silently dropped despite the listening window's
// own comment claiming otherwise. Returns 1 if this frame was an
// OFS_START_UPDATE for SELF (the listening window uses this to know it
// must stop listening and enter update mode instead of eventually
// falling through to JumpToApplication).
static uint8_t DispatchFrame(SpiOtaFrame_t *in, uint8_t tx_wire[SPI_FRAME_SIZE]) {
    SpiOtaFrame_t out;
    if (in->target_tier == SPI_TARGET_STACKA) {
        Relay_ToStackA(in->target_slot, in, &out);
        FrameToWire(&out, tx_wire);
    } else if (in->target_tier == SPI_TARGET_CM7) {
        Relay_ToCM7(in, &out);
        FrameToWire(&out, tx_wire);
    } else { // SPI_TARGET_SELF
        if (in->frame_type == OFS_START_UPDATE && in->dlc == 8) {
            return HandleStartUpdate(in->payload); // 1 only when the request was accepted
        } else if (in->frame_type == OFS_HMAC_CHUNK && in->dlc == 8) {
            HandleHmacChunk(in->payload);
        } else if (in->frame_type == OFS_IMAGE_HEADER && in->dlc == 8) {
            HandleImageHeaderChunk(in->payload);
        } else if (in->frame_type == OFS_DOWNGRADE_TOKEN && in->dlc == 8) {
            HandleDowngradeTokenChunk(in->payload);
        } else if (in->frame_type == OFS_DOWNGRADE_NONCE_REQUEST && in->dlc == 1) {
            HandleDowngradeNonceRequest(in->payload, in->dlc);
        } else if (in->frame_type == OFS_DATA) {
            HandleData(in->payload, in->dlc);
        } else if (in->frame_type == OFS_END_UPDATE && in->dlc == 8) {
            HandleEndUpdate(in->payload);
        } else if (in->frame_type == OFS_QUERY_VERSION) {
            HandleVersionQuery();
        } else if (in->frame_type == OFS_QUERY_ERROR_COUNTERS) {
            HandleErrorCounterQuery();
        } else if (in->frame_type == OFS_AUTHORIZE_DOWNGRADE && in->dlc == 4) {
            HandleAuthorizeDowngrade(in->payload);
        } else if (in->frame_type == OFS_BACKUP_READ_REQUEST) {
            HandleReadbackStart();
        } else if (in->frame_type == OFS_CONFIRM_HEALTHY) {
            HandleConfirmHealthy();
        }
#if HYDRA_SPI_SYSTEM
        else if (SpiSys_IsCommand(in->frame_type)) {
            // The read-only commands of spi_system.h. The bootloader has no emergency stop and no slot scan of its own, so stop, release and probe do nothing here.
            const SpiSysConfig_t config = SpiSys_ConfigFromBuild();
            SpiSysSnapshot_t snap = {0};
            snap.crc_errors = spi_crc_errors;
            snap.retries = spi_retries;
            snap.seq_gaps = spi_seq_gaps;
            snap.can_tx_errors = (uint8_t)(hfdcan1.Instance->ECR & 0xFFu);
            snap.can_rx_errors = (uint8_t)((hfdcan1.Instance->ECR >> 8) & 0x7Fu);
            snap.state = SPI_SYS_STATE_BOOTLOADER;
            snap.link_ok = (boot_hal_fault & BOOT_FAULT_LINK_MASK) == 0;
            uint8_t reply_type;
            uint8_t reply[8];
            uint8_t reply_dlc;
            if (SpiSys_Handle(&config, &snap, in->frame_type, in->payload, in->dlc, &reply_type, reply, &reply_dlc) == SPI_SYS_ACT_REPLY) {
                Protocol_StageSelfReply(reply_type, reply, reply_dlc);
            }
        }
#endif
    }
    return 0;
}

static uint32_t last_heartbeat_tick = 0 - 1000;

int main(void) {
    // IWDG armed as the very first action in main(), before HAL_Init() -
    // if HAL_Init()/SystemClock_Config() themselves ever got stuck, the
    // watchdog is already ticking and will still recover the board
    // instead of leaving the chip wedged with no independent watchdog
    // running yet. Safe this early: IWDG2 runs off its own always-on LSI
    // domain (no RCC/clock-tree dependency). HAL_IWDG_Init() waits for the
    // PVU/RVU register-update flags and does time that wait with
    // HAL_GetTick(); before HAL_Init() the tick does not advance, so that
    // timeout cannot fire here - the wait ends when the hardware clears the
    // flags (a few LSI cycles) and would spin on if it never did.
    hiwdg.Instance = IWDG2; // CM4 owns IWDG2 (CM7 owns IWDG1) - independent per-core watchdogs, docs/architecture.md section 2
    hiwdg.Init.Prescaler = IWDG_PRESCALER_32;
    hiwdg.Init.Reload = 4095; // generous placeholder, same TODO as CM7's own bootloader_main.c
    hiwdg.Init.Window = IWDG_WINDOW_DISABLE; // full-range window: a zero window would reset the chip on the first refresh
    if (HAL_IWDG_Init(&hiwdg) != HAL_OK) Boot_WatchdogFailed();

    HAL_Init();

    // What kind of reset brought the board here, read before anything clears it (see boot_decision.h): only a watchdog reset counts against the application.
    const uint8_t reset_kind = (__HAL_RCC_C2_GET_FLAG(RCC_FLAG_IWDG2RST) || __HAL_RCC_C2_GET_FLAG(RCC_FLAG_WWDG2RST)) ? BOOT_RESET_WATCHDOG : BOOT_RESET_CLEAN;
    __HAL_RCC_C2_CLEAR_RESET_FLAGS(); // this core's own copy of the reset status: the other core still sees its own
    SystemClock_Config();
    __HAL_RCC_HSEM_CLK_ENABLE();

    MX_SPI1_Init();
    MX_FDCAN1_Init();
    const uint8_t link_ok = (boot_hal_fault & BOOT_FAULT_LINK_MASK) == 0; // false: the SPI or the bus is not usable, a valid application still starts but no update can come in

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
    while (link_ok && (HAL_GetTick() - listen_start) < 600) {
        HAL_IWDG_Refresh(&hiwdg);
        Ipc_Pump(&IPC_HAL_OPS, 0, IPC_ROLE_GATEWAY);   // finish the handshake of the last frame exchanged with the CM7
        HAL_GPIO_WritePin(HYDRA_DATA_READY_PORT, HYDRA_DATA_READY_PIN, GPIO_PIN_SET);
        if (HAL_SPI_TransmitReceive(&hspi1, tx_wire, rx_wire, SPI_FRAME_SIZE, 20) == HAL_OK) {
            SpiOtaFrame_t in;
            if (Spi_AcceptFrame(rx_wire, &in) && DispatchFrame(&in, tx_wire)) {
                enter_update_mode = 1;
                break;
            }
        }
        HAL_GPIO_WritePin(HYDRA_DATA_READY_PORT, HYDRA_DATA_READY_PIN, GPIO_PIN_RESET);

        if (HAL_GetTick() - last_heartbeat_tick >= 1000) {
            last_heartbeat_tick = HAL_GetTick();
            CAN_SendHeartbeat(STATUS_LISTENING, 0xFF);
        }
        SpiOtaFrame_t pending;
        if (Protocol_TakePendingResponse(&pending)) FrameToWire(&pending, tx_wire);
    }

    // see mcu_stm32g474/boot/bootloader_main.c's own
    // comment for the full design.
    uint8_t should_jump = 0;
    if (!enter_update_mode && app_valid) {
        FirmwareMetadata_t boot_meta;
        uint8_t have_meta = Metadata_Read(&boot_meta) && boot_meta.magic == METADATA_MAGIC_VALID;
        const uint32_t stored_attempts = have_meta ? boot_meta.boot_attempts : 0;
        const uint32_t boot_attempts = BootDecision_EffectiveAttempts(stored_attempts, reset_kind);
        should_jump = BootDecision_ShouldJump(app_valid, boot_attempts);
        if (should_jump && have_meta) {
            const uint32_t next_attempts = BootDecision_NextAttempts(boot_attempts, reset_kind);
            if (next_attempts != stored_attempts) {   // a clean start with the count already at 0 writes nothing
                boot_meta.boot_attempts = next_attempts;
                Metadata_EraseAndWrite(&boot_meta);
            }
        }
    }

    if (should_jump) {
        JumpToApplication();
    } else if (!enter_update_mode && app_valid && link_ok) {
        CAN_SendHeartbeat(STATUS_ROLLBACK_SUSPECT, 0xFF);
    }

    if (!link_ok) {
        // No usable link and no application to start: stop refreshing the watchdog so the reset tries the whole set-up again.
        __disable_irq();
        for (;;) { }
    }

    CAN_SendStatus(STATUS_LISTENING);
    while (1) {
        HAL_IWDG_Refresh(&hiwdg);
        Ipc_Pump(&IPC_HAL_OPS, 0, IPC_ROLE_GATEWAY);   // finish the handshake of the last frame exchanged with the CM7
        HAL_GPIO_WritePin(HYDRA_DATA_READY_PORT, HYDRA_DATA_READY_PIN, GPIO_PIN_SET);
        if (HAL_SPI_TransmitReceive(&hspi1, tx_wire, rx_wire, SPI_FRAME_SIZE, 20) == HAL_OK) {
            SpiOtaFrame_t in;
            if (Spi_AcceptFrame(rx_wire, &in)) DispatchFrame(&in, tx_wire);
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
