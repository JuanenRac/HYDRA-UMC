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
#include <string.h>

static FDCAN_HandleTypeDef hfdcan1_app; /* uplink - STACK A, shared with the Kinematic Brain and every other slot */
static FDCAN_HandleTypeDef hfdcan2_app; /* downlink - this robot's own URTC Tool Head, Tier 2 */
static uint32_t slot_base;

/* Application-level offsets - architecture.md section 4's own reserved
 * range, mirrors KinematicBrainCan.c's identical #defines on the Tier 0
 * side (the query/response pair this module answers). */
#define OFS_AXIS_STATUS 0x10

/* -----------------------------------------------------------------------
 * BOARD_ID[2:0] - real, intentional duplicate of boot/bootloader_main.c's
 * own ReadSlotBaseId() (see this file's own header for why it can't just
 * be linked instead). Pull convention, pins, and formula are identical to
 * that function - re-verify both together if either ever changes,
 * docs/PINOUT_STM32G474_ROBOT_CONTROLLER.TXT section 1c is the shared
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
static void MX_FDCAN1_App_Init(void)
{
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
    hfdcan1_app.Init.StdFiltersNbr = 1;
    hfdcan1_app.Init.ExtFiltersNbr = 0;
    hfdcan1_app.Init.TxFifoQueueMode = FDCAN_TX_FIFO_OPERATION;
    HAL_FDCAN_Init(&hfdcan1_app);

    FDCAN_FilterTypeDef sf;
    sf.IdType = FDCAN_STANDARD_ID;
    sf.FilterIndex = 0;
    sf.FilterType = FDCAN_FILTER_MASK;
    sf.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
    sf.FilterID1 = 0x000;
    sf.FilterID2 = 0x000; /* wide open, same reasoning as the bootloader's own identical filter - this application checks the ID in software */
    HAL_FDCAN_ConfigFilter(&hfdcan1_app, &sf);
    HAL_FDCAN_ConfigGlobalFilter(&hfdcan1_app, FDCAN_ACCEPT_IN_RX_FIFO0, FDCAN_ACCEPT_IN_RX_FIFO0, FDCAN_REJECT_REMOTE, FDCAN_REJECT_REMOTE);

    HAL_FDCAN_Start(&hfdcan1_app);
}

/* Real FDCAN2 init - PB5=RX/PB6=TX (AF9), PG8=CAN2_STBY, the downlink to
 * this robot's own URTC Tool Head (docs/PINOUT_STM32G474_ROBOT_CONTROLLER.TXT
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
    HAL_FDCAN_Init(&hfdcan2_app);

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
    HAL_FDCAN_ConfigFilter(&hfdcan2_app, &sf2);
    HAL_FDCAN_ConfigGlobalFilter(&hfdcan2_app, FDCAN_ACCEPT_IN_RX_FIFO0, FDCAN_ACCEPT_IN_RX_FIFO0, FDCAN_REJECT_REMOTE, FDCAN_REJECT_REMOTE);

    HAL_FDCAN_Start(&hfdcan2_app);
}

/* -----------------------------------------------------------------------
 * AXIS_STATUS (+0x10) - real endstop/fault GPIO reads, docs/
 * CANBUS_STM32G474.TXT's own documented txData[0]/txData[1] layout.
 * Endstops: active-low with internal pull-up (docs/PINOUT_
 * STM32G474_ROBOT_CONTROLLER.TXT section 3, matching the Kinematic
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
    return 2; /* real DLC - docs/CANBUS_STM32G474.TXT only documents txData[0:1] for this frame */
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
    MX_FDCAN1_App_Init();
    MX_FDCAN2_Init();
    AxisStatusGpio_Init();
}

/* Answers one message of this board's own slot window on FDCAN1 (uplink). Every other slot's own traffic (and every other board's STACK A heartbeats) is silently ignored,
 * same "wide-open hardware filter, check in software" reasoning as the bootloader. */
static void ServiceUplinkMessage(uint32_t rx_id, const uint8_t rx_data[8], uint8_t rx_dlc)
{
    if (rx_id < slot_base || rx_id > slot_base + 0x1F) return;
    uint32_t offset = rx_id - slot_base;

    if (offset == OFS_AXIS_STATUS) {
        uint8_t out[8];
        uint8_t dlc = ReadAxisStatusFrame(out);
        SendClassicFrame(&hfdcan1_app, slot_base + RelayAnswerOffset(OFS_AXIS_STATUS), out, dlc); /* its own identifier: the request is the brain's */
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

    /* offset == OFS_ENTER_BOOTLOADER, or any other in-range-but-unhandled
     * offset: intentionally ignored - see this module's own .h header on
     * why ENTER_BOOTLOADER is a real, named, not-yet-implemented gap
     * rather than silently pretended-working. */
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
}
