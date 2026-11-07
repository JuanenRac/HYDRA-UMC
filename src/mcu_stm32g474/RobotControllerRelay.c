/*
 * =============================================================================
 * RobotControllerRelay.c - Real FDCAN1 slot responder + Tier 2/3 relay tunnel
 * PROJECT: HYDRA-UMC (Robot Controller Board firmware, Tier 1 - STM32G474RET6)
 * AUTHOR: JuanenRac (Electro Hobby 3D) - electrohobby3d@gmail.com
 * LICENSE: GPL-3.0-or-later - see repo root LICENSE
 *
 * See RobotControllerRelay.h's own header for the real scope/reasoning.
 * FDCAN1 pin/peripheral config below is a direct copy of the bootloader's
 * own already-proven MX_FDCAN1_Init() (boot/bootloader_main.c) - same real
 * init, reused rather than re-derived, since this application and that
 * bootloader never run at the same time (this project's own established
 * precedent - src/mcu_stm32h745/CM4/KinematicBrainCan.c does the exact
 * same thing for Tier 0's own FDCAN1). ReadSlotBaseId() below is likewise
 * a real, intentional duplicate of the bootloader's own function of the
 * same name - not an extern reference to it, since the bootloader and
 * this application are two separate linked binaries (each with its own
 * main()), so a symbol defined in boot/bootloader_main.c is never actually
 * linked into this application's own .elf.
 * =============================================================================
 */
#include "RobotControllerRelay.h"
#include "relay_rx_queue.h"
#include "relay_ids.h"
#include "relay_send_reassembly.h"
#include "dlc_to_fdcan_data_length.h"
#include "stm32g4xx_hal.h"
#include "boot/bootloader_common.h" /* shared CAN_ID_STACKA_BASE/STACKA_SLOT_WINDOW/OFS_* */
#include "boot/slot_scan_node.h"     /* the answer to the slot scan probe (docs/SLOT_SCAN.md) */
#include "estop_can.h"               /* the emergency stop on identifier 0x001 (docs/ESTOP_CAN.md) */
#include "estop_input.h"             /* the physical stop input and the stop category of each axis */
#include "machine_config.h"          /* generated from config/machine.cfg */
#include "fdcan_clock.h"            /* the explicit kernel clock of the FDCAN (off by default) */
#include "app_boot_request.h"      /* ENTER_BOOTLOADER answered by the application (off by default) */
#include "health_frame.h"           /* the once-a-second health frame (off by default) */
#include "boot_report.h"            /* what the bootloader found at this start, for the health flags (off by default) */
#include "init_fault.h"             /* what a failed set-up call of this application does: one reset, then the safe state (off by default) */
#include <string.h>

static FDCAN_HandleTypeDef hfdcan1_app; /* uplink - STACK A, shared with the Kinematic Brain and every other slot */
static FDCAN_HandleTypeDef hfdcan2_app; /* downlink - this robot's own URTC Tool Head, Tier 2 */
static uint32_t slot_base;
static uint8_t slot_board_id;         /* BOARD_ID[2:0], the slot this board answers for */
static uint32_t chip_uid32;           /* the 96-bit unique identifier of the chip folded to 32 bits, what the slot scan reply carries */
static SlotScanNode_t slot_scan_node; /* the reply to a probe, waiting for its turn */
static EstopNode_t estop_node;        /* LATCHED at power-up; READY only after a release the brain confirmed */
static uint8_t estop_state_due;       /* an ESTOP_STATE answer is owed to the kinematic brain */
static uint8_t estop_last_latched = 1;
static uint32_t estop_state_last_ms;
#define ESTOP_STATE_HEARTBEAT_MS 100u
static void EstopInputGpio_Init(void);
static EstopInput_t estop_input;      /* the physical stop loop, normally closed; starts tripped */
static StopPolicy_t stop_policy;      /* which axes are cut at once and which decelerate first */
static uint16_t estop_local_seq;      /* sequence of the stop frames this board sends from its own input */
static uint32_t estop_local_sent_ms;
static uint8_t estop_local_sent_once;
static HealthFrame_t health;          /* what the health frame reports (health_frame.h) */

/* Application-level offsets - architecture.md section 4's own reserved
 * range, mirrors KinematicBrainCan.c's identical #defines on the Tier 0
 * side (the query/response pair this module answers). */
#define OFS_AXIS_STATUS 0x10

/* -----------------------------------------------------------------------
 * BOARD_ID[2:0] - real, intentional duplicate of boot/bootloader_main.c's
 * own ReadSlotBaseId() (see this file's own header for why it can't just
 * be linked instead). Pull convention, pins, and formula are identical to
 * that function - re-verify both together if either ever changes,
 * docs/PINOUT_STM32G474_ROBOT_CONTROLLER.md section 1c is the shared
 * source of truth for both.
 * ----------------------------------------------------------------------- */
static uint32_t ReadSlotBaseId_App(void)
{
    __HAL_RCC_GPIOC_CLK_ENABLE();
    GPIO_InitTypeDef gi = {0};
    gi.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2;
    gi.Mode = GPIO_MODE_INPUT;
    gi.Pull = GPIO_PULLDOWN;
    HAL_GPIO_Init(GPIOC, &gi);

    uint32_t n = 0;
    if (HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_0) == GPIO_PIN_SET) n |= 0x1;
    if (HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_1) == GPIO_PIN_SET) n |= 0x2;
    if (HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_2) == GPIO_PIN_SET) n |= 0x4;
    return CAN_ID_STACKA_BASE + (n * STACKA_SLOT_WINDOW);
}

/* Real FDCAN1 init - PB8=RX/PB9=TX (AF9), PG7=CAN1_STBY - byte-for-byte
 * the bootloader's own MX_FDCAN1_Init() (see this file's own header). */
static uint8_t init_fault_prior;      /* failed set-ups recorded before this start (init_fault.h) */
static uint8_t uplink_ready;          /* FDCAN1 is started: a frame can still leave if a later set-up call fails */
static uint8_t SendClassicFrame(FDCAN_HandleTypeDef *hfdcan, uint32_t can_id, const uint8_t *data, uint8_t dlc);

/* A set-up call of the peripherals that fails leaves the board unable to answer the brain. With HYDRA_INIT_FAULT_LATCH off: stop here with the interrupts off, nothing
 * refreshes the watchdog any more, it resets the chip into the bootloader, whose boot-attempt count decides whether this application is tried again. With it on: the
 * first failure asks for one reset, the second leaves the drivers off and refreshes the watchdog for ever (init_fault.h). */
static void RelayHalRequire(HAL_StatusTypeDef status)
{
    if (status != HAL_OK) {
        MotionOutputs_ForceDisabled();
        switch (InitFault_OnFailure(&TAMP->BKP4R, init_fault_prior)) {
        case INIT_FAULT_RESET:
            __disable_irq();
            NVIC_SystemReset();
            for (;;) { }
        case INIT_FAULT_SAFE_STATE:
#if HYDRA_HEALTH_FRAME
            if (uplink_ready) {
                /* the uplink works and a later call failed: say so once, then hold still (the frame is the only thing sent from here on) */
                uint8_t health_bytes[8];
                health.flags = (uint8_t)(health.flags | InitFault_SafeStateFlags());
                HealthFrame_Build(&health, health_bytes);
                (void)SendClassicFrame(&hfdcan1_app, slot_base + HEALTH_OFFSET, health_bytes, 8);
            }
#endif
            __disable_irq();
            for (;;) { AppWatchdog_Refresh(); }
        default:
            __disable_irq();
            for (;;) { }
        }
    }
}

/* STACK A: the window of 32 identifiers of this slot (mask 0x7E0) and the emergency stop (0x001). */
#define RELAY_SLOT_FILTER_MASK 0x7E0u
#define RELAY_ESTOP_CAN_ID     0x001u

static void MX_FDCAN1_App_Init(void)
{
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOG_CLK_ENABLE();
#if HYDRA_FDCAN_KERNEL_CLOCK_EXPLICIT
    {   /* the selector resets to the HSE, which does not run: take the APB1 clock the bit timing assumes (fdcan_clock.h) */
        RCC_PeriphCLKInitTypeDef fdcan_clock = {0};
        fdcan_clock.PeriphClockSelection = RCC_PERIPHCLK_FDCAN;
        fdcan_clock.FdcanClockSelection = RCC_FDCANCLKSOURCE_PCLK1;
        RelayHalRequire(HAL_RCCEx_PeriphCLKConfig(&fdcan_clock));
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
    HAL_GPIO_WritePin(GPIOG, GPIO_PIN_7, GPIO_PIN_RESET);

    hfdcan1_app.Instance = FDCAN1;
    hfdcan1_app.Init.FrameFormat = FDCAN_FRAME_CLASSIC;
    hfdcan1_app.Init.Mode = FDCAN_MODE_NORMAL;
    hfdcan1_app.Init.AutoRetransmission = ENABLE;
    hfdcan1_app.Init.TransmitPause = DISABLE;
    hfdcan1_app.Init.ProtocolException = DISABLE;
    hfdcan1_app.Init.NominalPrescaler = 1;
    hfdcan1_app.Init.NominalSyncJumpWidth = 1;
    hfdcan1_app.Init.NominalTimeSeg1 = 13;
    hfdcan1_app.Init.NominalTimeSeg2 = 2;
    hfdcan1_app.Init.DataPrescaler = 1;
    hfdcan1_app.Init.DataSyncJumpWidth = 1;
    hfdcan1_app.Init.DataTimeSeg1 = 13;
    hfdcan1_app.Init.DataTimeSeg2 = 2;
    hfdcan1_app.Init.StdFiltersNbr = 2;
    hfdcan1_app.Init.ExtFiltersNbr = 0;
    hfdcan1_app.Init.TxFifoQueueMode = FDCAN_TX_FIFO_OPERATION;
    RelayHalRequire(HAL_FDCAN_Init(&hfdcan1_app));

    FDCAN_FilterTypeDef sf;
    sf.IdType = FDCAN_STANDARD_ID;
    sf.FilterIndex = 0;
    sf.FilterType = FDCAN_FILTER_MASK;
    sf.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
    sf.FilterID1 = slot_base; /* same two filters as the bootloader: this slot's window and the emergency stop, everything else rejected by the peripheral */
    sf.FilterID2 = RELAY_SLOT_FILTER_MASK;
    RelayHalRequire(HAL_FDCAN_ConfigFilter(&hfdcan1_app, &sf));
    sf.FilterIndex = 1;
    sf.FilterID1 = RELAY_ESTOP_CAN_ID;
    sf.FilterID2 = 0x7FF; /* exact match */
    RelayHalRequire(HAL_FDCAN_ConfigFilter(&hfdcan1_app, &sf));
    RelayHalRequire(HAL_FDCAN_ConfigGlobalFilter(&hfdcan1_app, FDCAN_REJECT, FDCAN_REJECT, FDCAN_REJECT_REMOTE, FDCAN_REJECT_REMOTE));

    RelayHalRequire(HAL_FDCAN_Start(&hfdcan1_app));
}

/* Real FDCAN2 init - PB5=RX/PB6=TX (AF9), PG8=CAN2_STBY, the downlink to
 * this robot's own URTC Tool Head (docs/PINOUT_STM32G474_ROBOT_CONTROLLER.md
 * section 1b - "standard G474 mapping - re-verify in CubeMX", unlike
 * FDCAN1's Table-12-confirmed pins). Same Classic-CAN bit timing as
 * FDCAN1: URTC's own bxCAN protocol (sibling repo, docs/CANBUS.TXT) is a
 * fixed-bitrate bus this board's transceiver must match, and this
 * project's own established STACK A rate (~1 Mbps at the same assumed
 * 16MHz kernel clock, see MX_FDCAN1_App_Init's own real bit-timing
 * derivation) is reused here rather than inventing a second, undocumented
 * rate - re-verify against URTC's own real bus speed once real hardware
 * for both boards exists to measure it on.
 */
static void MX_FDCAN2_Init(void)
{
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOG_CLK_ENABLE();
    __HAL_RCC_FDCAN_CLK_ENABLE();

    GPIO_InitTypeDef gi = {0};
    gi.Pin = GPIO_PIN_5 | GPIO_PIN_6;
    gi.Mode = GPIO_MODE_AF_PP;
    gi.Pull = GPIO_NOPULL;
    gi.Speed = GPIO_SPEED_FREQ_HIGH;
    gi.Alternate = GPIO_AF9_FDCAN2;
    HAL_GPIO_Init(GPIOB, &gi);

    GPIO_InitTypeDef gi_stby = {0};
    gi_stby.Pin = GPIO_PIN_8;
    gi_stby.Mode = GPIO_MODE_OUTPUT_PP;
    gi_stby.Pull = GPIO_NOPULL;
    gi_stby.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOG, &gi_stby);
    HAL_GPIO_WritePin(GPIOG, GPIO_PIN_8, GPIO_PIN_RESET);

    hfdcan2_app.Instance = FDCAN2;
    hfdcan2_app.Init.FrameFormat = FDCAN_FRAME_CLASSIC;
    hfdcan2_app.Init.Mode = FDCAN_MODE_NORMAL;
    hfdcan2_app.Init.AutoRetransmission = ENABLE;
    hfdcan2_app.Init.TransmitPause = DISABLE;
    hfdcan2_app.Init.ProtocolException = DISABLE;
    hfdcan2_app.Init.NominalPrescaler = 1;
    hfdcan2_app.Init.NominalSyncJumpWidth = 1;
    hfdcan2_app.Init.NominalTimeSeg1 = 13;
    hfdcan2_app.Init.NominalTimeSeg2 = 2;
    hfdcan2_app.Init.DataPrescaler = 1;
    hfdcan2_app.Init.DataSyncJumpWidth = 1;
    hfdcan2_app.Init.DataTimeSeg1 = 13;
    hfdcan2_app.Init.DataTimeSeg2 = 2;
    hfdcan2_app.Init.StdFiltersNbr = 1;
    hfdcan2_app.Init.ExtFiltersNbr = 0;
    hfdcan2_app.Init.TxFifoQueueMode = FDCAN_TX_FIFO_OPERATION;
    RelayHalRequire(HAL_FDCAN_Init(&hfdcan2_app));

    /* Wide open too - this board's own URTC head is the ONLY thing on
     * FDCAN2, but URTC's own protocol spans over a hundred IDs
     * (0x000-0x2FF runtime, 0x7F0-0x7FF bootloader), so a software check
     * would gain nothing a hardware filter could narrow safely anyway -
     * every frame the URTC head sends is real cargo for RELAY_RECV to
     * eventually deliver, this board never discards any of it. */
    FDCAN_FilterTypeDef sf2;
    sf2.IdType = FDCAN_STANDARD_ID;
    sf2.FilterIndex = 0;
    sf2.FilterType = FDCAN_FILTER_MASK;
    sf2.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
    sf2.FilterID1 = 0x000;
    sf2.FilterID2 = 0x000;
    RelayHalRequire(HAL_FDCAN_ConfigFilter(&hfdcan2_app, &sf2));
    RelayHalRequire(HAL_FDCAN_ConfigGlobalFilter(&hfdcan2_app, FDCAN_ACCEPT_IN_RX_FIFO0, FDCAN_ACCEPT_IN_RX_FIFO0, FDCAN_REJECT_REMOTE, FDCAN_REJECT_REMOTE));

    RelayHalRequire(HAL_FDCAN_Start(&hfdcan2_app));
}

/* -----------------------------------------------------------------------
 * AXIS_STATUS (+0x10) - real endstop/fault GPIO reads, docs/
 * CANBUS_STM32G474.md's own documented txData[0]/txData[1] layout.
 * Endstops: active-low with internal pull-up (docs/PINOUT_
 * STM32G474_ROBOT_CONTROLLER.md section 3, matching the Kinematic
 * Brain's own convention) - triggered = pin reads LOW.
 * DRV_DIAG_OR/PGOOD: open-drain/wired-OR fault-sense lines (section 2a/4
 * of that same pinout file); this project has no confirmed active-high/
 * active-low polarity documented for the specific regulator/driver parts
 * yet, so this reads them with the same active-low-with-internal-pull-up
 * convention as the endstops (the overwhelmingly standard real-world
 * convention for an open-drain fault/PG line: released HIGH when
 * healthy, actively pulled LOW to report a fault) - re-verify against the
 * real regulator/TMC5160A datasheets once a schematic exists, same
 * "PROPOSED, re-verify before schematic capture" caveat this pinout
 * file's own header already carries for every non-Table-12-confirmed pin.
 * ----------------------------------------------------------------------- */
static void AxisStatusGpio_Init(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    GPIO_InitTypeDef gi = {0};
    gi.Mode = GPIO_MODE_INPUT;
    gi.Pull = GPIO_PULLUP;
    gi.Speed = GPIO_SPEED_FREQ_LOW;

    gi.Pin = GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_10 | GPIO_PIN_11; /* endstops 1,2,3,4 */
    HAL_GPIO_Init(GPIOB, &gi);
    gi.Pin = GPIO_PIN_4 | GPIO_PIN_3; /* endstops 5,6 */
    HAL_GPIO_Init(GPIOC, &gi);

    gi.Pin = GPIO_PIN_2; /* DRV_DIAG_OR */
    HAL_GPIO_Init(GPIOA, &gi);
    gi.Pin = GPIO_PIN_7 | GPIO_PIN_3; /* PGOOD_5V, PGOOD_3V3 */
    HAL_GPIO_Init(GPIOB, &gi);
}

static uint8_t ReadAxisStatusFrame(uint8_t out[8])
{
    memset(out, 0, 8);

    uint8_t endstop_mask = 0;
    if (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_1) == GPIO_PIN_RESET) endstop_mask |= (1u << 0);
    if (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_2) == GPIO_PIN_RESET) endstop_mask |= (1u << 1);
    if (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_10) == GPIO_PIN_RESET) endstop_mask |= (1u << 2);
    if (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_11) == GPIO_PIN_RESET) endstop_mask |= (1u << 3);
    if (HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_4) == GPIO_PIN_RESET) endstop_mask |= (1u << 4);
    if (HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_3) == GPIO_PIN_RESET) endstop_mask |= (1u << 5);

    uint8_t fault_flags = 0;
    if (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_2) == GPIO_PIN_RESET) fault_flags |= (1u << 0); /* DRV_DIAG_OR asserted */
    if (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_7) == GPIO_PIN_RESET) fault_flags |= (1u << 1); /* PGOOD_5V lost */
    if (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_3) == GPIO_PIN_RESET) fault_flags |= (1u << 2); /* PGOOD_3V3 lost */

    out[0] = endstop_mask;
    out[1] = fault_flags;
    return 2; /* real DLC - docs/CANBUS_STM32G474.md only documents txData[0:1] for this frame */
}

/* -----------------------------------------------------------------------
 * Real Classic CAN send/receive helpers - identical shape to
 * KinematicBrainCan.c's own (same DlcToFdcanDataLength() table - see
 * dlc_to_fdcan_data_length.h, A.11 - same padded-8-byte
 * AddMessageToTxFifoQ call), parameterized over WHICH FDCAN peripheral
 * so the same helpers serve both FDCAN1 (uplink) and FDCAN2 (downlink)
 * without duplicating this logic a second time.
 * ----------------------------------------------------------------------- */

static uint8_t SendClassicFrame(FDCAN_HandleTypeDef *hfdcan, uint32_t can_id, const uint8_t *data, uint8_t dlc)
{
    FDCAN_TxHeaderTypeDef tx = {0};
    tx.Identifier = can_id;
    tx.IdType = FDCAN_STANDARD_ID;
    tx.TxFrameType = FDCAN_DATA_FRAME;
    tx.DataLength = DlcToFdcanDataLength(dlc);
    tx.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
    tx.BitRateSwitch = FDCAN_BRS_OFF;
    tx.FDFormat = FDCAN_CLASSIC_CAN;
    tx.TxEventFifoControl = FDCAN_NO_TX_EVENTS;
    tx.MessageMarker = 0;

    uint8_t padded[8] = {0};
    if (dlc > 0) memcpy(padded, data, dlc);
    return HAL_FDCAN_AddMessageToTxFifoQ(hfdcan, &tx, padded) == HAL_OK;
}

/* Non-blocking: returns 1 and fills out_* if a frame is already waiting,
 * 0 immediately if not - this module's own poll loop drives the "when to
 * check again" timing, no per-call busy-wait like KinematicBrainCan.c's
 * own WaitForResponse() (that side is a bus MASTER issuing a request and
 * waiting on a specific reply; this side is two independent RESPONDERS -
 * one per bus - servicing whatever request/frame arrives next, so
 * blocking here would stall the other bus's own servicing for no
 * benefit). */
static uint8_t TryGetMessage(FDCAN_HandleTypeDef *hfdcan, uint32_t *out_id, uint8_t *out_data, uint8_t *out_dlc)
{
    if (HAL_FDCAN_GetRxFifoFillLevel(hfdcan, FDCAN_RX_FIFO0) == 0) return 0;
    FDCAN_RxHeaderTypeDef rx;
    uint8_t rx_data[8];
    if (HAL_FDCAN_GetRxMessage(hfdcan, FDCAN_RX_FIFO0, &rx, rx_data) != HAL_OK) return 0;
    uint8_t dlc = (uint8_t)FdcanDataLengthToDlc(rx.DataLength); /* the HAL already shifted the DLC field down: DataLength is the plain 0..15 code */
    *out_id = rx.Identifier;
    *out_dlc = dlc;
    memcpy(out_data, rx_data, dlc);
    return 1;
}

/* -----------------------------------------------------------------------
 * FDCAN2 (downlink) capture FIFO - real, bounded (no dynamic allocation,
 * matching this project's own embedded convention throughout). Filled by
 * RobotControllerRelay_Poll() draining FDCAN2's own hardware RX FIFO
 * every pass; drained, oldest first, by RELAY_RECV requests from Tier 0.
 * 8 deep: generous relative to how fast a single operator (HYDRA-UMC-
 * STUDIO) polls RELAY_RECV, deeper than the 3 frames this board's own
 * FDCAN2 hardware RX FIFO0 holds before this software layer even gets
 * involved (RobotControllerRelay_Poll empties that FIFO every pass).
 * ----------------------------------------------------------------------- */
/* The ring buffer itself (struct, RELAY_RX_QUEUE_DEPTH, Push/Pop, the
 * drop-oldest-when-full policy) now lives in relay_rx_queue.h - a HAL-free
 * header so it can be regression-tested on the host with plain gcc. The
 * `static inline` functions there generate exactly the code this had when
 * it was written inline here; only the single-instance state moved into a
 * struct. See tests/test_relay_rx_queue.c. */
static RelayRxQueue_t relay_rx_queue;
/* Holds the first fragment of a RELAY_SEND frame until its second one arrives. */
static RelaySendReassembly_t relay_send_reassembly;

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */
void RobotControllerRelay_Init(void)
{
    RelayRxQueue_Reset(&relay_rx_queue);
    RelaySendReassembly_Reset(&relay_send_reassembly);
    slot_base = ReadSlotBaseId_App();
    slot_board_id = (uint8_t)((slot_base - CAN_ID_STACKA_BASE) / STACKA_SLOT_WINDOW);
    chip_uid32 = SlotScan_FoldUid(HAL_GetUIDw0(), HAL_GetUIDw1(), HAL_GetUIDw2());
    SlotScanNode_Reset(&slot_scan_node);
    HealthFrame_Reset(&health);
#if HYDRA_INIT_FAULT_LATCH
    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_RCC_RTCAPB_CLK_ENABLE();
    HAL_PWR_EnableBkUpAccess();
#endif
    init_fault_prior = InitFault_Begin(&TAMP->BKP4R);
    health.flags = (uint8_t)(health.flags | InitFault_HealthFlags(init_fault_prior));
#if HYDRA_BOOT_REPORT
    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_RCC_RTCAPB_CLK_ENABLE();
    HAL_PWR_EnableBkUpAccess();
    health.flags = (uint8_t)(health.flags | BootReport_TakeHealthFlags(&TAMP->BKP2R));
#endif
    EstopNode_Reset(&estop_node);
    EstopInputGpio_Init();
    StopPolicy_Init(&stop_policy, (uint8_t)MACHINE_STOP_CATEGORY1_MASK, MACHINE_STOP_RAMP_MAX_MS);
    MotionOutputs_ForceDisabled(); /* power-up is LATCHED: the driver enables are off until a release and the normal enable command */
    (void)StopPolicy_Trip(&stop_policy, HAL_GetTick());
    stop_policy.cut_mask = (uint8_t)((1u << STOP_AXES) - 1u); /* everything is already off at power-up */
    estop_state_due = 1;
    MX_FDCAN1_App_Init();
    uplink_ready = 1; /* from here a failed set-up call can still be announced to the brain */
    MX_FDCAN2_Init();
    AxisStatusGpio_Init();
    InitFault_Clear(&TAMP->BKP4R); /* the set-up finished: forget the failed starts (the health flag of this start stays) */
}

/* Answers one message of this board's own slot window on FDCAN1 (uplink). Every other slot's own traffic (and every other board's STACK A heartbeats) is silently ignored,
 * same "wide-open hardware filter, check in software" reasoning as the bootloader. */
/* The physical stop input (machine.cfg, section estop_input): the pull is opposite to the healthy level, so a cut wire reads as the stop level. */
static GPIO_TypeDef *EstopInputPort(void)
{
    switch (MACHINE_ESTOP_INPUT_PORT) {
    case 'A': return GPIOA;
    case 'B': return GPIOB;
    default:  return GPIOC;
    }
}

static void EstopInputGpio_Init(void)
{
    const EstopInputConfig_t cfg = {
        MACHINE_ESTOP_INPUT_ENABLED, MACHINE_ESTOP_INPUT_HEALTHY_LEVEL,
        MACHINE_ESTOP_INPUT_TRIP_DEBOUNCE_MS, MACHINE_ESTOP_INPUT_CLEAR_DEBOUNCE_MS,
    };
    EstopInput_Init(&estop_input, &cfg);
    if (!cfg.enabled) return;
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    GPIO_InitTypeDef gi = {0};
    gi.Pin = (uint16_t)(1u << MACHINE_ESTOP_INPUT_PIN);
    gi.Mode = GPIO_MODE_INPUT;
    gi.Pull = EstopInput_PullUp(&cfg) ? GPIO_PULLUP : GPIO_PULLDOWN;
    gi.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(EstopInputPort(), &gi);
}

/* Applies the stop category of each axis while the node is latched: the category 0 axes are cut at once, the category 1 axes after their ramp at the
 * latest. The cuts are written again every pass. A READY node forgets the stop. */
static void EstopApplyOutputs(uint32_t now)
{
    if (!estop_node.latched) {
        StopPolicy_Clear(&stop_policy);
        return;
    }
    (void)StopPolicy_Trip(&stop_policy, now);
    (void)StopPolicy_Tick(&stop_policy, now);
    MotionOutputs_ForceDisabledMask(stop_policy.cut_mask);
}

/* The loop is open: the stop frame goes out as a TRIP (reason hardware input) on the stack bus and to the tool head, and the node is latched through the
 * same decision as a stop heard on the bus, so whichever fires first latches. Repeated every MACHINE_ESTOP_INPUT_TRIP_REPEAT_MS. */
static void EstopLocalTrip(uint32_t now, uint8_t announce)
{
    uint8_t frame[8];
    Estop_Encode(frame, ESTOP_KIND_TRIP, ESTOP_REASON_HARDWARE, (uint8_t)(ESTOP_SOURCE_SLOT_FIRST + slot_board_id), estop_node.epoch, estop_local_seq);
    EstopNode_OnFrame(&estop_node, now, frame, 8);
    EstopApplyOutputs(now); /* the drivers go off before anything is transmitted */
    if (!announce) return;
    if (estop_local_sent_once && (uint32_t)(now - estop_local_sent_ms) < MACHINE_ESTOP_INPUT_TRIP_REPEAT_MS) return;
    estop_local_sent_ms = now;
    estop_local_sent_once = 1;
    estop_local_seq++;
    SendClassicFrame(&hfdcan1_app, RELAY_ESTOP_CAN_ID, frame, 8);
    SendClassicFrame(&hfdcan2_app, RELAY_ESTOP_CAN_ID, frame, 8);
}

/* A frame on the emergency stop identifier: the node state machine decides, the drivers go off at once when it is latched, and the frame is passed
 * on unchanged to the tool head on the downlink (one frame in, one frame out, never inside the relay tunnel). */
static void ServiceEstopFrame(const uint8_t data[8], uint8_t dlc)
{
    const uint8_t length = dlc > 8u ? 8u : dlc;
    const uint32_t now = HAL_GetTick();
    EstopNode_OnFrame(&estop_node, now, data, length);
    if (estop_input.tripped) EstopLocalTrip(now, 0); /* a RELEASE cannot unlatch the board while the loop is open */
    EstopApplyOutputs(now);
    SendClassicFrame(&hfdcan2_app, RELAY_ESTOP_CAN_ID, data, length);
    if (length == 8u && data[0] == ESTOP_KIND_RELEASE) estop_state_due = 1; /* the brain waits for this answer to every RELEASE */
}

/* Once per pass: the silence of the master latches a READY node, a bus-off latches it and restarts the controller, a latched node keeps the drivers
 * off, and the ESTOP_STATE answer goes out on a change, after a RELEASE and every ESTOP_STATE_HEARTBEAT_MS. */
static void EstopHousekeeping(void)
{
    const uint32_t now = HAL_GetTick();
    EstopNode_Tick(&estop_node, now);
    if (MACHINE_ESTOP_INPUT_ENABLED) {
        const uint8_t raw = (uint8_t)(HAL_GPIO_ReadPin(EstopInputPort(), (uint16_t)(1u << MACHINE_ESTOP_INPUT_PIN)) == GPIO_PIN_SET);
        if (EstopInput_Sample(&estop_input, now, raw)) EstopLocalTrip(now, 1);
        else estop_local_sent_once = 0;
    }
    FDCAN_ProtocolStatusTypeDef protocol;
    if (HAL_FDCAN_GetProtocolStatus(&hfdcan1_app, &protocol) == HAL_OK && protocol.BusOff) {
        EstopNode_OnBusOff(&estop_node);
        CLEAR_BIT(hfdcan1_app.Instance->CCCR, FDCAN_CCCR_INIT); /* start the recovery; the bus coming back releases nothing */
    }
    EstopApplyOutputs(now);
    if (estop_node.latched != estop_last_latched) {
        estop_last_latched = estop_node.latched;
        estop_state_due = 1;
    }
    if ((uint32_t)(now - estop_state_last_ms) >= ESTOP_STATE_HEARTBEAT_MS) estop_state_due = 1;
    if (estop_state_due) {
        uint8_t state[4];
        EstopNode_BuildState(&estop_node, state);
        if (SendClassicFrame(&hfdcan1_app, slot_base + OFS_ESTOP_STATE, state, 4)) {
            estop_state_due = 0;
            estop_state_last_ms = now;
        }
    }
}

uint8_t RobotControllerRelay_EstopReady(void)
{
    return (uint8_t)!estop_node.latched;
}

uint8_t RobotControllerRelay_RequestDriversEnable(void)
{
    return EstopNode_Enable(&estop_node);
}

uint8_t RobotControllerRelay_ScanReplyPending(void)
{
    return slot_scan_node.pending;
}

static void ServiceUplinkMessage(uint32_t rx_id, const uint8_t rx_data[8], uint8_t rx_dlc)
{
    if (rx_id == RELAY_ESTOP_CAN_ID) {
        ServiceEstopFrame(rx_data, rx_dlc);
        return;
    }
    if (rx_id < slot_base || rx_id > slot_base + 0x1F) return;
    uint32_t offset = rx_id - slot_base;

    if (offset == OFS_AXIS_STATUS) {
        uint8_t out[8];
        uint8_t dlc = ReadAxisStatusFrame(out);
        SendClassicFrame(&hfdcan1_app, slot_base + RelayAnswerOffset(OFS_AXIS_STATUS), out, dlc); /* its own identifier: the request is the brain's */
        return;
    }

    if (offset == OFS_SLOT_SCAN_PROBE) {
        /* who is in this slot: the answer leaves after a delay taken from the unique identifier of the chip, see RobotControllerRelay_Poll */
        (void)SlotScanNode_OnProbe(&slot_scan_node, HAL_GetTick(), rx_data, rx_dlc, chip_uid32);
        return;
    }

    if (offset == OFS_QUERY_ERROR_COUNTERS) {
        /* the same answer the bootloader gives, read from the uplink controller itself */
        uint8_t counters[2];
        RelayErrorCounters(hfdcan1_app.Instance->ECR, counters);
        SendClassicFrame(&hfdcan1_app, slot_base + OFS_ERROR_COUNTERS_RESPONSE, counters, 2);
        return;
    }

    if (offset == OFS_RELAY_SEND) {
        /* RELAY_SEND fragment - matches KinematicBrainCan.c's own
         * SendRelayFragments() byte-for-byte: rx_data[0:2] = target CAN ID
         * (BE), rx_data[2] = the TOTAL data length of the frame being
         * relayed, rx_data[3:] = up to 5 data bytes. A frame of more than 5
         * bytes arrives as two fragments; the tool head's bootloader only
         * accepts whole frames (8 bytes, or 4 for the readback request), so
         * the fragments are put back together here and leave on FDCAN2 as
         * ONE frame (see relay_send_reassembly.h, which also drops target
         * IDs above 0x7FF). */
        if (rx_dlc < RELAY_FRAGMENT_HEADER_BYTES) return; /* malformed - too short to even carry a target ID */
        uint16_t target_id = ((uint16_t)rx_data[0] << 8) | rx_data[1];
        uint8_t total_dlc = rx_data[2];
        uint8_t fragment_len = (uint8_t)(rx_dlc - RELAY_FRAGMENT_HEADER_BYTES);
        uint16_t out_id;
        uint8_t out_data[8];
        uint8_t out_dlc;
        if (RelaySendReassembly_Feed(&relay_send_reassembly, HAL_GetTick(), target_id, total_dlc,
                                     &rx_data[RELAY_FRAGMENT_HEADER_BYTES], fragment_len,
                                     &out_id, out_data, &out_dlc)) {
            SendClassicFrame(&hfdcan2_app, out_id, out_data, out_dlc);
        }
        return;
    }

    if (offset == OFS_RELAY_RECV) {
        /* Real, pull-based drain (docs/architecture.md section 5): every
         * RELAY_RECV request drains exactly ONE queued frame, fragmented
         * into the same 2-fragment-max shape RELAY_SEND uses - matches
         * KinematicBrainCan.c's own DrainRelayRecv() expectations on the
         * polling side (it re-polls until its own accumulated_dlc target
         * is met, so replying with 0 bytes here when the queue is empty,
         * rather than blocking, is the correct real "nothing new yet"
         * signal - it just re-polls again). */
        RelayCapturedFrame_t frame;
        if (!RelayRxQueue_Pop(&relay_rx_queue, &frame)) return; /* nothing queued - real, expected idle state, not an error */

        RelayAnswerFragments_t answer;
        RelayBuildAnswerFragments(&frame, &answer);
        for (uint8_t i = 0; i < answer.count; i++) {
            SendClassicFrame(&hfdcan1_app, slot_base + RelayAnswerOffset(OFS_RELAY_RECV), answer.data[i], answer.dlc[i]);
        }
        return;
    }

#if HYDRA_APP_ENTER_BOOTLOADER
    if (offset == OFS_ENTER_BOOTLOADER) {
        /* Hand the board over to its bootloader (app_boot_request.h): only with the exact magic and only while the drivers are off. */
        __HAL_RCC_PWR_CLK_ENABLE();
        __HAL_RCC_RTCAPB_CLK_ENABLE();
        HAL_PWR_EnableBkUpAccess();
        if (AppBoot_OnEnterBootloader(&TAMP->BKP0R, rx_data, rx_dlc, estop_node.latched) == APP_BOOT_ACCEPTED) {
            HAL_NVIC_SystemReset();   /* the bootloader reads the request and stays in its update window */
        }
        return;
    }
#endif
    /* Any other in-range-but-unhandled offset is ignored. ENTER_BOOTLOADER is ignored too unless the build sets HYDRA_APP_ENTER_BOOTLOADER. */
}

/* How many messages one pass takes out of each FDCAN receive FIFO. The hardware FIFO of the STM32G4's FDCAN holds only 3 frames and the task sleeps between passes, so
 * taking one message per pass lost frames in silence when a burst (the fragments of an HMAC transfer, the tool head's telemetry) arrived within one sleep. */
#define RELAY_POLL_BURST 8

void RobotControllerRelay_Poll(void)
{
    /* 1. Drain FDCAN2 (downlink) into the capture queue - real traffic from this robot's own URTC Tool Head, buffered until RELAY_RECV drains it. Takes up to
     *    RELAY_POLL_BURST messages, so a burst larger than the hardware FIFO is not lost between two passes. */
    for (int taken = 0; taken < RELAY_POLL_BURST; taken++) {
        uint32_t id;
        uint8_t data[8];
        uint8_t dlc;
        if (!TryGetMessage(&hfdcan2_app, &id, data, &dlc)) break;
        RelayRxQueue_Push(&relay_rx_queue, (uint16_t)id, data, dlc);
    }

    /* 2. Service FDCAN1 (uplink), the same way. */
    for (int taken = 0; taken < RELAY_POLL_BURST; taken++) {
        uint32_t rx_id;
        uint8_t rx_data[8];
        uint8_t rx_dlc;
        if (!TryGetMessage(&hfdcan1_app, &rx_id, rx_data, &rx_dlc)) break;
        ServiceUplinkMessage(rx_id, rx_data, rx_dlc);
    }

    /* 3. The emergency stop bookkeeping and the reply to the slot scan probe once its time has come. */
    EstopHousekeeping();
#if HYDRA_HEALTH_FRAME
    {
        uint8_t health_bytes[8];
        uint8_t counters[2];
        health.flags = (uint8_t)((health.flags & ~(HEALTH_FLAG_ESTOP | HEALTH_FLAG_ENABLED)) | (estop_node.latched ? HEALTH_FLAG_ESTOP : HEALTH_FLAG_ENABLED));
        RelayErrorCounters(hfdcan1_app.Instance->ECR, counters);
        health.tx_errors = counters[0];
        health.rx_errors = counters[1];
        if (HealthFrame_Due(&health, HAL_GetTick(), health_bytes)) {
            SendClassicFrame(&hfdcan1_app, slot_base + HEALTH_OFFSET, health_bytes, 8);
        }
    }
#endif
    uint8_t scan_reply[8];
    if (SlotScanNode_Due(&slot_scan_node, HAL_GetTick(), slot_board_id, SLOT_SCAN_ROLE_APPLICATION, chip_uid32, scan_reply)) {
        SendClassicFrame(&hfdcan1_app, slot_base + OFS_SLOT_SCAN_REPLY, scan_reply, 8);
    }
}
