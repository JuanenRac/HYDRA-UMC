// =============================================================================
// HYDRA-UMC Robot Controller Board Bootloader - clock config, FDCAN init,
// BOARD_ID read, main loop, and interrupt handlers
// Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
// GPL-3.0-or-later - see repo root LICENSE
//
// Ported from URTC's own proven bootloader_main.c (same repo, src/F303-
// master/boot/) - same listening-window/update/heartbeat/timeout structure.
// Real differences: FDCAN1 instead of bxCAN (see MX_FDCAN1_Init below), and
// every ID this bootloader sends/listens for is offset from this board's
// own BOARD_ID-derived base (ReadSlotBaseId(), read once at startup from a
// LOCAL DIP switch - not the STACK A connector) instead of a fixed
// absolute ID - see docs/architecture.md section 4 and docs/PINOUT_
// STM32G474_ROBOT_CONTROLLER.md sections 1a/1c for the pins and
// addressing scheme this file implements.
//
// Pin assignments below match docs/PINOUT_STM32G474_ROBOT_CONTROLLER.md
// exactly - re-verify there (and against Table 12 of the datasheet) before
// changing anything here, not the other way around.
//
// SystemClock_Config() below still runs on the HSI reset default (16MHz,
// no PLL) - same "not yet a real clock tree" TODO flagged throughout this
// project (docs/COMPILE_STM32G474.md, FreeRTOSConfig.h's own header
// comment). The FDCAN bit-timing below (Prescaler=1, NominalTimeSeg1=13,
// NominalTimeSeg2=2, NominalSyncJumpWidth=1 -> 16 time quanta/bit at
// 16MHz = 1Mbps) is derived FROM that 16MHz assumption specifically to hit
// README.md section 6's own stated "1 Mbps Arbitration Bitrate" for STACK A
// - if/when a real clock tree changes the FDCAN kernel clock, this bit
// timing needs revisiting together with it, not independently.
// =============================================================================
#include "stm32g4xx_hal.h"
#include "bootloader_common.h"
#include "bootloader_crypto.h"
#include "bootloader_flash.h"
#include "bootloader_protocol.h"
#include "boot_decision.h"
#include "dlc_to_fdcan_data_length.h"
#include "slot_scan_node.h"
#include "fdcan_clock.h"        // the explicit kernel clock of the FDCAN (off by default)
#include "app_boot_request.h"  // an application asking to be updated (off by default)
#include "boot_report.h"        // what this start found, handed to the application for its health frame (off by default)

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

// STACK A: this board listens to its own window of 32 identifiers (slot_base .. slot_base + 0x1F, mask 0x7E0) and to the emergency stop (0x001).
#define BOOT_SLOT_FILTER_MASK 0x7E0u
#define BOOT_ESTOP_CAN_ID     0x001u

void SystemClock_Config(void) {
    // HSI (16MHz) reset default, no PLL - TODO once a real clock tree is
    // designed (see this file's own header comment). HAL_Init() already
    // leaves the chip in this state; this call is a placeholder for where
    // the real config goes, kept explicit rather than omitted so it's
    // obvious this step was considered, not forgotten.
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
    RCC_OscInitStruct.HSIState = RCC_HSI_ON;
    RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    RCC_OscInitStruct.PLL.PLLState = RCC_PLL_NONE;
    BOOT_HAL_NOTE(HAL_RCC_OscConfig(&RCC_OscInitStruct), BOOT_FAULT_CLOCK);

    RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                                 | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
    BOOT_HAL_NOTE(HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0), BOOT_FAULT_CLOCK);
}

// -----------------------------------------------------------------------
// BOARD_ID[2:0] - docs/PINOUT_STM32G474_ROBOT_CONTROLLER.md section 1c:
// PC0=LSB, PC1, PC2=MSB, a LOCAL 3-position DIP switch on this board's own
// PCB (manually set by the installer, not derived from the STACK A
// connector or stack position). Read once at startup (a DIP switch can't
// change while the board is powered), converted to this board's own
// FDCAN1 slot base ID.
// -----------------------------------------------------------------------
uint32_t ReadSlotBaseId(void) {
    __HAL_RCC_GPIOC_CLK_ENABLE();
    GPIO_InitTypeDef gi = {0};
    gi.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2;
    gi.Mode = GPIO_MODE_INPUT;
    gi.Pull = GPIO_PULLDOWN; // DIP switch pulls high per closed position to encode N - open/unset reads as address 0, a safe default rather than undefined
    HAL_GPIO_Init(GPIOC, &gi);

    uint32_t n = 0;
    if (HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_0) == GPIO_PIN_SET) n |= 0x1;
    if (HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_1) == GPIO_PIN_SET) n |= 0x2;
    if (HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_2) == GPIO_PIN_SET) n |= 0x4;
    return CAN_ID_STACKA_BASE + (n * STACKA_SLOT_WINDOW);
}

// -----------------------------------------------------------------------
// FDCAN1 init - PB8=FDCAN1_RX, PB9=FDCAN1_TX (AF9, CONFIRMED pins per
// docs/PINOUT_STM32G474_ROBOT_CONTROLLER.md section 1a), classic-CAN-only
// (FDCAN_FRAME_CLASSIC, BRS off) so every frame this protocol sends/
// receives has the same 11-bit-ID/0-8-byte-DLC shape URTC's own bxCAN
// protocol already assumes - this bootloader deliberately never uses any
// FD-only feature. PG7 (CAN1_STBY) driven low to bring the transceiver out
// of standby before FDCAN1 itself starts.
// -----------------------------------------------------------------------
static void MX_FDCAN1_Init(uint32_t slot_base) {
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOG_CLK_ENABLE();
#if HYDRA_FDCAN_KERNEL_CLOCK_EXPLICIT
    // The selector of the FDCAN kernel clock resets to the HSE, which is not running here: take the APB1 clock (the HSI at divider 1, 16 MHz), the
    // clock the bit timing below assumes (fdcan_clock.h). Off by default; turn it on after bench row 10.
    {
        RCC_PeriphCLKInitTypeDef fdcan_clock = {0};
        fdcan_clock.PeriphClockSelection = RCC_PERIPHCLK_FDCAN;
        fdcan_clock.FdcanClockSelection = RCC_FDCANCLKSOURCE_PCLK1;
        BOOT_HAL_NOTE(HAL_RCCEx_PeriphCLKConfig(&fdcan_clock), BOOT_FAULT_CLOCK);
    }
#endif
    __HAL_RCC_FDCAN_CLK_ENABLE();

    GPIO_InitTypeDef gi = {0};
    gi.Pin = GPIO_PIN_8 | GPIO_PIN_9;
    gi.Mode = GPIO_MODE_AF_PP;
    gi.Pull = GPIO_NOPULL;
    gi.Speed = GPIO_SPEED_FREQ_HIGH;
    gi.Alternate = GPIO_AF9_FDCAN1;
    HAL_GPIO_Init(GPIOB, &gi);

    GPIO_InitTypeDef gi_stby = {0};
    gi_stby.Pin = GPIO_PIN_7;
    gi_stby.Mode = GPIO_MODE_OUTPUT_PP;
    gi_stby.Pull = GPIO_NOPULL;
    gi_stby.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOG, &gi_stby);
    HAL_GPIO_WritePin(GPIOG, GPIO_PIN_7, GPIO_PIN_RESET); // active-low standby, per typical CAN transceiver STB convention (e.g. TCAN1044AVD)

    hfdcan1.Instance = FDCAN1;
    hfdcan1.Init.FrameFormat = FDCAN_FRAME_CLASSIC;
    hfdcan1.Init.Mode = FDCAN_MODE_NORMAL;
    hfdcan1.Init.AutoRetransmission = ENABLE;
    hfdcan1.Init.TransmitPause = DISABLE;
    hfdcan1.Init.ProtocolException = DISABLE;
    // 16 time quanta/bit at an assumed 16MHz FDCAN kernel clock = 1Mbps -
    // see this file's own header comment for why, and what changes this
    // needs revisiting alongside.
    hfdcan1.Init.NominalPrescaler = 1;
    hfdcan1.Init.NominalSyncJumpWidth = 1;
    hfdcan1.Init.NominalTimeSeg1 = 13;
    hfdcan1.Init.NominalTimeSeg2 = 2;
    // Unused with BRS off (classic frames only), but the HAL still range-
    // checks these - mirrored to the nominal values above.
    hfdcan1.Init.DataPrescaler = 1;
    hfdcan1.Init.DataSyncJumpWidth = 1;
    hfdcan1.Init.DataTimeSeg1 = 13;
    hfdcan1.Init.DataTimeSeg2 = 2;
    hfdcan1.Init.StdFiltersNbr = 2;
    hfdcan1.Init.ExtFiltersNbr = 0;
    // No MessageRAMOffset/RxFifoNElmts*/TxFifoQueueElmts*/TxElmtSize fields
    // on this HAL version (v1.2.7) - CONFIRMED by actually compiling
    // against it: FDCAN_InitTypeDef here is the simpler pre-explicit-
    // message-RAM-partitioning shape, message RAM layout is handled
    // internally with a fixed default rather than being caller-configured
    // the way later HAL versions (and the H7 family's own HAL) allow.
    hfdcan1.Init.TxFifoQueueMode = FDCAN_TX_FIFO_OPERATION;
    BOOT_HAL_NOTE(HAL_FDCAN_Init(&hfdcan1), BOOT_FAULT_CAN_INIT);
    if (boot_hal_fault & BOOT_FAULT_CAN_INIT) return; // the peripheral is not usable: no filter, no start

    // Hardware filters: only this board's own window of 32 identifiers (mask 0x7E0) and the emergency stop reach the receive FIFO; every
    // other identifier is rejected by the peripheral, so the other slots' traffic never costs this loop any time. The bootloader still
    // checks the offset in software after receiving.
    FDCAN_FilterTypeDef sf;
    sf.IdType = FDCAN_STANDARD_ID;
    sf.FilterIndex = 0;
    sf.FilterType = FDCAN_FILTER_MASK;
    sf.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
    sf.FilterID1 = slot_base;
    sf.FilterID2 = BOOT_SLOT_FILTER_MASK;
    BOOT_HAL_NOTE(HAL_FDCAN_ConfigFilter(&hfdcan1, &sf), BOOT_FAULT_CAN_FILTER);
    sf.FilterIndex = 1;
    sf.FilterID1 = BOOT_ESTOP_CAN_ID;
    sf.FilterID2 = 0x7FF; // exact match
    BOOT_HAL_NOTE(HAL_FDCAN_ConfigFilter(&hfdcan1, &sf), BOOT_FAULT_CAN_FILTER);
    BOOT_HAL_NOTE(HAL_FDCAN_ConfigGlobalFilter(&hfdcan1, FDCAN_REJECT, FDCAN_REJECT, FDCAN_REJECT_REMOTE, FDCAN_REJECT_REMOTE), BOOT_FAULT_CAN_FILTER);
    if (boot_hal_fault & BOOT_FAULT_CAN_FILTER) return; // never start with a filter that is not the one that was asked for

    BOOT_HAL_NOTE(HAL_FDCAN_Start(&hfdcan1), BOOT_FAULT_CAN_START);
}

static uint32_t last_heartbeat_tick = 0 - 1000; // forces an immediate heartbeat on the first check

// The slot scan (docs/SLOT_SCAN.md): the answer to a probe leaves after a delay taken from the unique identifier of the chip, so two boards set to the
// same slot answer one after the other and the host can tell that there are two.
static SlotScanNode_t boot_scan;
static uint32_t boot_uid32;
static uint8_t boot_board_id;

static void BootScanPoll(void) {
    uint8_t reply[8];
    if (SlotScanNode_Due(&boot_scan, HAL_GetTick(), boot_board_id, SLOT_SCAN_ROLE_BOOTLOADER, boot_uid32, reply)) {
        CAN_SendSlotScanReply(reply);
    }
}

int main(void) {
    // IWDG armed as the very first action in main(), before HAL_Init() -
    // if HAL_Init()/SystemClock_Config() themselves ever got stuck (bad
    // flash wait-state config, stuck oscillator-ready wait loop), the
    // watchdog is already ticking and will still recover the board,
    // instead of leaving the chip wedged with no independent watchdog
    // running yet. Safe to call this early: IWDG runs off its own
    // always-on LSI domain (no RCC/clock-tree dependency). HAL_IWDG_Init()
    // waits for the PVU/RVU register-update flags and does time that wait
    // with HAL_GetTick(); before HAL_Init() the tick does not advance, so
    // that timeout cannot fire here - the wait ends when the hardware clears
    // the flags (a few LSI cycles) and would spin on if it never did. Same IWDG
    // config shape as URTC's own bootloader (independent watchdog -
    // survives NVIC_SystemReset(), so a 0x-relative-ENTER_BOOTLOADER-
    // triggered reset inherits an already-ticking countdown). Prescaler/
    // Reload here are placeholders matching this project's own
    // FreeRTOSConfig.h note on LSI/clock-tree still being TODO - re-tune
    // once a real clock tree exists, same as URTC's own file flags for its
    // chip.
    hiwdg.Instance = IWDG;
    hiwdg.Init.Prescaler = IWDG_PRESCALER_32;
    hiwdg.Init.Reload = 999;
    hiwdg.Init.Window = IWDG_WINDOW_DISABLE; // full-range window: a zero window would reset the chip on the first refresh
    if (HAL_IWDG_Init(&hiwdg) != HAL_OK) Boot_WatchdogFailed();

    HAL_Init();

    // What kind of reset brought the board here, read before anything clears it (see boot_decision.h): only a watchdog reset counts against the application.
    const uint8_t reset_kind = (__HAL_RCC_GET_FLAG(RCC_FLAG_IWDGRST) || __HAL_RCC_GET_FLAG(RCC_FLAG_WWDGRST)) ? BOOT_RESET_WATCHDOG : BOOT_RESET_CLEAN;
    __HAL_RCC_CLEAR_RESET_FLAGS();
    SystemClock_Config();

    uint32_t slot_base = ReadSlotBaseId();
    Protocol_Init(slot_base);
    boot_board_id = (uint8_t)((slot_base - CAN_ID_STACKA_BASE) / STACKA_SLOT_WINDOW);
    boot_uid32 = SlotScan_FoldUid(HAL_GetUIDw0(), HAL_GetUIDw1(), HAL_GetUIDw2());
    SlotScanNode_Reset(&boot_scan);

    MX_FDCAN1_Init(slot_base);
    const uint8_t link_ok = (boot_hal_fault & BOOT_FAULT_LINK_MASK) == 0; // false: the bus is not usable, a valid application still starts but no update can come in

    uint8_t app_valid = ApplicationIsValid();

    uint32_t listen_start = HAL_GetTick();
    uint8_t enter_update_mode = 0;
    uint32_t listen_window_ms = 600;
#if HYDRA_APP_ENTER_BOOTLOADER
    // The application asked to be updated (app_boot_request.h): stay in the update window longer than the usual start-up window, once.
    {
        __HAL_RCC_PWR_CLK_ENABLE();
        __HAL_RCC_RTCAPB_CLK_ENABLE();
        HAL_PWR_EnableBkUpAccess();
        if (AppBoot_TakeRequest(&TAMP->BKP0R)) listen_window_ms = 10000;   // BKP0R and BKP1R are consecutive registers
    }
#endif

    while (link_ok && (HAL_GetTick() - listen_start) < listen_window_ms) {
        HAL_IWDG_Refresh(&hiwdg);
        BootScanPoll();
        if (HAL_FDCAN_GetRxFifoFillLevel(&hfdcan1, FDCAN_RX_FIFO0) > 0) {
            FDCAN_RxHeaderTypeDef rxHeader;
            uint8_t rxData[8];
            if (HAL_FDCAN_GetRxMessage(&hfdcan1, FDCAN_RX_FIFO0, &rxHeader, rxData) == HAL_OK) {
                if (rxHeader.RxFrameType != FDCAN_DATA_FRAME || rxHeader.IdType != FDCAN_STANDARD_ID) {
                    continue;
                }
                uint32_t ofs = (rxHeader.Identifier >= slot_base) ? (rxHeader.Identifier - slot_base) : 0xFFFF;
                uint32_t dlc = FdcanDataLengthToDlc(rxHeader.DataLength);
                if (ofs == OFS_START_UPDATE && dlc == 8) {
                    // Only an accepted request takes the board out of its normal
                    // start-up: a refused one (bad size or hardware ID, from any
                    // node on the bus) leaves the listening window running.
                    if (HandleStartUpdate(rxData)) {
                        enter_update_mode = 1;
                        break;
                    }
                } else if (ofs == OFS_QUERY_VERSION) {
                    HandleVersionQuery();
                } else if (ofs == OFS_SLOT_SCAN_PROBE) {
                    (void)SlotScanNode_OnProbe(&boot_scan, HAL_GetTick(), rxData, (uint8_t)dlc, boot_uid32);
                } else if (ofs == OFS_QUERY_ERROR_COUNTERS) {
                    HandleErrorCounterQuery();
                } else if (ofs == OFS_BACKUP_READ_REQUEST) {
                    HandleReadbackStart();
                } else if (ofs == OFS_CONFIRM_HEALTHY) {
                    HandleConfirmHealthy();
                }
            }
        }
        if (HAL_GetTick() - last_heartbeat_tick >= 1000) {
            last_heartbeat_tick = HAL_GetTick();
            CAN_SendHeartbeat(STATUS_LISTENING, 0xFF);
        }
    }

    // a cryptographically valid app is not necessarily a
    // WORKING one - see boot_decision.h's own header comment for the full
    // design. FirmwareMetadata_t's own boot_attempts is read fresh here
    // (never trusted from the app_valid check above, which only reads the
    // magic/state), incremented and persisted to flash BEFORE jumping (so
    // a crash-and-reset immediately after jump is captured even though
    // this bootloader itself never regains control otherwise), and only
    // jumped to while under BOOT_MAX_ATTEMPTS.
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
                Metadata_EraseAndWrite(&boot_meta); // best-effort: a write failure here still jumps (never worse than before this fix), just without the updated count persisted
            }
        }
    }

    if (should_jump) {
#if HYDRA_BOOT_REPORT
        // Tell the application what this start found (boot_report.h): a watchdog reset and a failed set-up call of the bootloader.
        __HAL_RCC_PWR_CLK_ENABLE();
        __HAL_RCC_RTCAPB_CLK_ENABLE();
        HAL_PWR_EnableBkUpAccess();
        BootReport_Write(&TAMP->BKP2R, (uint8_t)(reset_kind == BOOT_RESET_WATCHDOG), boot_hal_fault);   // BKP2R and BKP3R are consecutive registers
#endif
        JumpToApplication();
    } else if (!enter_update_mode && app_valid && link_ok) {
        // Refused only because of the real boot-attempt count above (a
        // structurally invalid app, or an explicit update request, was
        // already handled elsewhere) - stays recoverable over CAN instead
        // of silently crash-looping into the same suspect app forever.
        CAN_SendHeartbeat(STATUS_ROLLBACK_SUSPECT, 0xFF);
    }

    if (!link_ok) {
        // No usable bus and no application to start: stop refreshing the watchdog so the reset tries the whole set-up again.
        __disable_irq();
        for (;;) { }
    }

    CAN_SendStatus(STATUS_LISTENING);
    while (1) {
        HAL_IWDG_Refresh(&hiwdg);
        BootScanPoll();
        if (HAL_FDCAN_GetRxFifoFillLevel(&hfdcan1, FDCAN_RX_FIFO0) > 0) {
            FDCAN_RxHeaderTypeDef rxHeader;
            uint8_t rxData[8];
            if (HAL_FDCAN_GetRxMessage(&hfdcan1, FDCAN_RX_FIFO0, &rxHeader, rxData) == HAL_OK) {
                if (rxHeader.RxFrameType != FDCAN_DATA_FRAME || rxHeader.IdType != FDCAN_STANDARD_ID) {
                    continue;
                }
                uint32_t ofs = (rxHeader.Identifier >= slot_base) ? (rxHeader.Identifier - slot_base) : 0xFFFF;
                uint32_t dlc = FdcanDataLengthToDlc(rxHeader.DataLength);
                if (ofs == OFS_START_UPDATE && dlc == 8) {
                    HandleStartUpdate(rxData);
                } else if (ofs == OFS_HMAC_CHUNK && dlc == 8) {
                    HandleHmacChunk(rxData);
                } else if (ofs == OFS_IMAGE_HEADER && dlc == 8) {
                    HandleImageHeaderChunk(rxData);
                } else if (ofs == OFS_DOWNGRADE_TOKEN && dlc == 8) {
                    HandleDowngradeTokenChunk(rxData);
                } else if (ofs == OFS_DOWNGRADE_NONCE_REQUEST && dlc == 1) {
                    HandleDowngradeNonceRequest(rxData, dlc);
                } else if (ofs == OFS_DATA) {
                    HandleData(rxData, dlc);
                } else if (ofs == OFS_END_UPDATE && dlc == 8) {
                    HandleEndUpdate(rxData);
                } else if (ofs == OFS_QUERY_VERSION) {
                    HandleVersionQuery();
                } else if (ofs == OFS_SLOT_SCAN_PROBE) {
                    (void)SlotScanNode_OnProbe(&boot_scan, HAL_GetTick(), rxData, (uint8_t)dlc, boot_uid32);
                } else if (ofs == OFS_QUERY_ERROR_COUNTERS) {
                    HandleErrorCounterQuery();
                } else if (ofs == OFS_AUTHORIZE_DOWNGRADE && dlc == 4) {
                    HandleAuthorizeDowngrade(rxData);
                } else if (ofs == OFS_BACKUP_READ_REQUEST) {
                    HandleReadbackStart();
                } else if (ofs == OFS_CONFIRM_HEALTHY) {
                    HandleConfirmHealthy();
                }
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

        // Same 10s-of-silence-mid-transfer abort as URTC's own bootloader -
        // the backup slot was already erased by HandleStartUpdate, main
        // slot was never touched, so aborting back to listening is always
        // safe and lets the master just retry.
        if (update_in_progress && !update_failed &&
            (HAL_GetTick() - update_last_activity_tick > 10000)) {
            update_in_progress = 0;
            update_failed = 0;
            CAN_SendStatus(STATUS_LISTENING);
        }
    }
}

void HardFault_Handler(void) {
    // Same reasoning as URTC's own handler: force every pin on GPIOA/GPIOB
    // to a safe, disconnected state. This bootloader doesn't drive any
    // actuators itself, but if it's running at all something has already
    // gone wrong with the application, so erring toward "everything off"
    // costs nothing. 0xEBFFFFFF preserves PA13/PA14 (SWD).
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

// Same polling-not-interrupt-driven FDCAN reception as URTC's own
// bootloader (see that file's own note) - the hardware FIFO fills
// autonomously regardless of interrupt configuration, and a bootloader has
// nothing else competing for CPU time. HAL_GetTick() depends on SysTick
// actually incrementing it - without this handler, every HAL_Delay() call
// here (including the listening-window timeout check) would spin forever.
void SysTick_Handler(void) {
    HAL_IncTick();
}
