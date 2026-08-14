// =============================================================================
// HYDRA-UMC Robot Controller Board Bootloader - clock config, FDCAN init,
// BOARD_ID read, main loop, and interrupt handlers
// Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
// GPL-3.0 - see repo root LICENSE
//
// Ported from URTC's own proven bootloader_main.c (same repo, src/F303-
// master/boot/) - same listening-window/update/heartbeat/timeout structure.
// Real differences: FDCAN1 instead of bxCAN (see MX_FDCAN1_Init below), and
// every ID this bootloader sends/listens for is offset from this board's
// own BOARD_ID-derived base (ReadSlotBaseId(), read once at startup from a
// LOCAL DIP switch - not the STACK A connector) instead of a fixed
// absolute ID - see docs/architecture.md section 4 and docs/PINOUT_
// STM32G474_ROBOT_CONTROLLER.TXT sections 1a/1c for the pins and
// addressing scheme this file implements.
//
// Pin assignments below match docs/PINOUT_STM32G474_ROBOT_CONTROLLER.TXT
// exactly - re-verify there (and against Table 12 of the datasheet) before
// changing anything here, not the other way around.
//
// SystemClock_Config() below still runs on the HSI reset default (16MHz,
// no PLL) - same "not yet a real clock tree" TODO flagged throughout this
// project (docs/COMPILE_STM32G474.TXT, FreeRTOSConfig.h's own header
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

FDCAN_HandleTypeDef hfdcan1;
IWDG_HandleTypeDef  hiwdg;

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
    HAL_RCC_OscConfig(&RCC_OscInitStruct);

    RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                                 | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
    HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0);
}

// -----------------------------------------------------------------------
// BOARD_ID[2:0] - docs/PINOUT_STM32G474_ROBOT_CONTROLLER.TXT section 1c:
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
// docs/PINOUT_STM32G474_ROBOT_CONTROLLER.TXT section 1a), classic-CAN-only
// (FDCAN_FRAME_CLASSIC, BRS off) so every frame this protocol sends/
// receives has the same 11-bit-ID/0-8-byte-DLC shape URTC's own bxCAN
// protocol already assumes - this bootloader deliberately never uses any
// FD-only feature. PG7 (CAN1_STBY) driven low to bring the transceiver out
// of standby before FDCAN1 itself starts.
// -----------------------------------------------------------------------
static void MX_FDCAN1_Init(void) {
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOG_CLK_ENABLE();
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
    hfdcan1.Init.StdFiltersNbr = 1;
    hfdcan1.Init.ExtFiltersNbr = 0;
    // No MessageRAMOffset/RxFifoNElmts*/TxFifoQueueElmts*/TxElmtSize fields
    // on this HAL version (v1.2.7) - CONFIRMED by actually compiling
    // against it: FDCAN_InitTypeDef here is the simpler pre-explicit-
    // message-RAM-partitioning shape, message RAM layout is handled
    // internally with a fixed default rather than being caller-configured
    // the way later HAL versions (and the H7 family's own HAL) allow.
    hfdcan1.Init.TxFifoQueueMode = FDCAN_TX_FIFO_OPERATION;
    HAL_FDCAN_Init(&hfdcan1);

    // Wide-open filter, same reasoning as URTC's own bootloader: this
    // bootloader only ever cares about its own slot_base+0x00..0x1F window
    // and just checks the ID in software after receiving - narrowing the
    // hardware filter isn't worth the risk of a bit-alignment mistake here
    // any more than it was there. Unmatched frames still need SOMEWHERE to
    // go, so they're routed to Rx FIFO0 like everything else rather than
    // rejected, per HAL_FDCAN_ConfigGlobalFilter's own reject-vs-accept
    // default-filter arguments below.
    FDCAN_FilterTypeDef sf;
    sf.IdType = FDCAN_STANDARD_ID;
    sf.FilterIndex = 0;
    sf.FilterType = FDCAN_FILTER_MASK;
    sf.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
    sf.FilterID1 = 0x000;
    sf.FilterID2 = 0x000; // mask=0 -> match anything
    HAL_FDCAN_ConfigFilter(&hfdcan1, &sf);
    HAL_FDCAN_ConfigGlobalFilter(&hfdcan1, FDCAN_ACCEPT_IN_RX_FIFO0, FDCAN_ACCEPT_IN_RX_FIFO0, FDCAN_REJECT_REMOTE, FDCAN_REJECT_REMOTE);

    HAL_FDCAN_Start(&hfdcan1);
}

static uint32_t last_heartbeat_tick = 0 - 1000; // forces an immediate heartbeat on the first check

int main(void) {
    HAL_Init();
    SystemClock_Config();

    uint32_t slot_base = ReadSlotBaseId();
    Protocol_Init(slot_base);

    MX_FDCAN1_Init();

    // Same IWDG config shape as URTC's own bootloader (independent watchdog
    // - survives NVIC_SystemReset(), so a 0x-relative-ENTER_BOOTLOADER-
    // triggered reset inherits an already-ticking countdown). Prescaler/
    // Reload here are placeholders matching this project's own
    // FreeRTOSConfig.h note on LSI/clock-tree still being TODO - re-tune
    // once a real clock tree exists, same as URTC's own file flags for its
    // chip.
    hiwdg.Instance = IWDG;
    hiwdg.Init.Prescaler = IWDG_PRESCALER_32;
    hiwdg.Init.Reload = 999;
    HAL_IWDG_Init(&hiwdg);

    uint8_t app_valid = ApplicationIsValid();

    uint32_t listen_start = HAL_GetTick();
    uint8_t enter_update_mode = 0;

    while ((HAL_GetTick() - listen_start) < 600) {
        HAL_IWDG_Refresh(&hiwdg);
        if (HAL_FDCAN_GetRxFifoFillLevel(&hfdcan1, FDCAN_RX_FIFO0) > 0) {
            FDCAN_RxHeaderTypeDef rxHeader;
            uint8_t rxData[8];
            if (HAL_FDCAN_GetRxMessage(&hfdcan1, FDCAN_RX_FIFO0, &rxHeader, rxData) == HAL_OK) {
                if (rxHeader.RxFrameType != FDCAN_DATA_FRAME || rxHeader.IdType != FDCAN_STANDARD_ID) {
                    continue;
                }
                uint32_t ofs = (rxHeader.Identifier >= slot_base) ? (rxHeader.Identifier - slot_base) : 0xFFFF;
                uint32_t dlc = rxHeader.DataLength >> 16;
                if (ofs == OFS_START_UPDATE && dlc == 8) {
                    enter_update_mode = 1;
                    HandleStartUpdate(rxData);
                    break;
                } else if (ofs == OFS_QUERY_VERSION) {
                    HandleVersionQuery();
                } else if (ofs == OFS_QUERY_ERROR_COUNTERS) {
                    HandleErrorCounterQuery();
                } else if (ofs == OFS_BACKUP_READ_REQUEST) {
                    HandleReadbackStart();
                }
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
        if (HAL_FDCAN_GetRxFifoFillLevel(&hfdcan1, FDCAN_RX_FIFO0) > 0) {
            FDCAN_RxHeaderTypeDef rxHeader;
            uint8_t rxData[8];
            if (HAL_FDCAN_GetRxMessage(&hfdcan1, FDCAN_RX_FIFO0, &rxHeader, rxData) == HAL_OK) {
                if (rxHeader.RxFrameType != FDCAN_DATA_FRAME || rxHeader.IdType != FDCAN_STANDARD_ID) {
                    continue;
                }
                uint32_t ofs = (rxHeader.Identifier >= slot_base) ? (rxHeader.Identifier - slot_base) : 0xFFFF;
                uint32_t dlc = rxHeader.DataLength >> 16;
                if (ofs == OFS_START_UPDATE && dlc == 8) {
                    HandleStartUpdate(rxData);
                } else if (ofs == OFS_HMAC_CHUNK && dlc == 8) {
                    HandleHmacChunk(rxData);
                } else if (ofs == OFS_DATA) {
                    HandleData(rxData, dlc);
                } else if (ofs == OFS_END_UPDATE && dlc == 8) {
                    HandleEndUpdate(rxData);
                } else if (ofs == OFS_QUERY_VERSION) {
                    HandleVersionQuery();
                } else if (ofs == OFS_QUERY_ERROR_COUNTERS) {
                    HandleErrorCounterQuery();
                } else if (ofs == OFS_AUTHORIZE_DOWNGRADE && dlc == 4) {
                    HandleAuthorizeDowngrade(rxData);
                } else if (ofs == OFS_BACKUP_READ_REQUEST) {
                    HandleReadbackStart();
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
