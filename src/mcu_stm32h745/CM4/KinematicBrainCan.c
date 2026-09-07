/*
 * =============================================================================
 * KinematicBrainCan.c - Real FDCAN1 "STACK A" master + Tier 2/3 relay tunnel
 * PROJECT: HYDRA-UMC (Kinematic Brain firmware, Tier 0 - STM32H745ZIT6)
 * AUTHOR: JuanenRac (Electro Hobby 3D) - electrohobby3d@gmail.com
 * LICENSE: GPL-3.0 - see repo root LICENSE
 *
 * See KinematicBrainCan.h's own header for the real scope/reasoning.
 * FDCAN1 pin/peripheral config below is a direct copy of the bootloader's
 * own already-proven MX_FDCAN1_Init() (../boot/bootloader_main.c) - same
 * real init, reused rather than re-derived, since this application and
 * that bootloader never run at the same time (one owns the peripheral
 * only after JumpToApplication() hands off, matching every other tier's
 * own bootloader-then-application handoff in this project).
 * =============================================================================
 */
#include "KinematicBrainCan.h"
#include "stm32h7xx_hal.h"
#include "bootloader_common.h" /* shared CAN_ID_STACKA_BASE/STACKA_SLOT_WINDOW/OFS_* - see that header's own comment on why this application reuses it, not a copy */
#include <string.h>

static FDCAN_HandleTypeDef hfdcan1_app;

/* +0x10/+0x11 - new application-level offsets, architecture.md section 4's
 * own reserved range for this (the bootloader-era offsets +0x00..+0x0F/
 * +0x12..+0x14 are all already spoken for by the real bootloader protocol
 * every tier shares). */
#define OFS_AXIS_STATUS          0x10
#define OFS_AXIS_STEP_TELEMETRY  0x11

static uint32_t StackASlotBaseId(uint8_t slot)
{
    return CAN_ID_STACKA_BASE + ((uint32_t)slot * STACKA_SLOT_WINDOW);
}

void KinematicBrainCan_Init(void)
{
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

    hfdcan1_app.Instance = FDCAN1;
    hfdcan1_app.Init.FrameFormat = FDCAN_FRAME_CLASSIC;
    hfdcan1_app.Init.Mode = FDCAN_MODE_NORMAL;
    hfdcan1_app.Init.AutoRetransmission = ENABLE;
    hfdcan1_app.Init.TransmitPause = DISABLE;
    hfdcan1_app.Init.ProtocolException = DISABLE;
    /* Same real prescaler/timing this application inherits from the
     * bootloader's own MX_FDCAN1_Init() - see that function's own comment
     * on the kernel-clock assumption this is built on. */
    hfdcan1_app.Init.NominalPrescaler = 1;
    hfdcan1_app.Init.NominalSyncJumpWidth = 1;
    hfdcan1_app.Init.NominalTimeSeg1 = 13;
    hfdcan1_app.Init.NominalTimeSeg2 = 2;
    hfdcan1_app.Init.DataPrescaler = 1;
    hfdcan1_app.Init.DataSyncJumpWidth = 1;
    hfdcan1_app.Init.DataTimeSeg1 = 13;
    hfdcan1_app.Init.DataTimeSeg2 = 2;
    hfdcan1_app.Init.MessageRAMOffset = 0;
    hfdcan1_app.Init.StdFiltersNbr = 1;
    hfdcan1_app.Init.ExtFiltersNbr = 0;
    hfdcan1_app.Init.RxFifo0ElmtsNbr = 8;
    hfdcan1_app.Init.RxFifo0ElmtSize = FDCAN_DATA_BYTES_8;
    hfdcan1_app.Init.RxFifo1ElmtsNbr = 0;
    hfdcan1_app.Init.RxBuffersNbr = 0;
    hfdcan1_app.Init.TxEventsNbr = 0;
    hfdcan1_app.Init.TxBuffersNbr = 0;
    hfdcan1_app.Init.TxFifoQueueElmtsNbr = 8;
    hfdcan1_app.Init.TxFifoQueueMode = FDCAN_TX_FIFO_OPERATION;
    hfdcan1_app.Init.TxElmtSize = FDCAN_DATA_BYTES_8;
    HAL_FDCAN_Init(&hfdcan1_app);

    FDCAN_FilterTypeDef sf;
    sf.IdType = FDCAN_STANDARD_ID;
    sf.FilterIndex = 0;
    sf.FilterType = FDCAN_FILTER_MASK;
    sf.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
    sf.FilterID1 = 0x000;
    sf.FilterID2 = 0x000; /* wide open - this core sees traffic from all 8 STACK A slots, same reasoning as the bootloader's own identical filter */
    HAL_FDCAN_ConfigFilter(&hfdcan1_app, &sf);
    HAL_FDCAN_ConfigGlobalFilter(&hfdcan1_app, FDCAN_ACCEPT_IN_RX_FIFO0, FDCAN_ACCEPT_IN_RX_FIFO0, FDCAN_REJECT_REMOTE, FDCAN_REJECT_REMOTE);

    HAL_FDCAN_Start(&hfdcan1_app);
}

/* DLC 0-8 map directly onto FDCAN_DLC_BYTES_0..8 for Classic CAN (no
 * FDCAN_DLC_BYTES_12/16/... escape codes apply below DLC 8) - a real,
 * closed lookup, not a formula, since the raw enum values aren't a simple
 * DLC*shift relationship in the real HAL headers. */
static uint32_t DlcToFdcanDataLength(uint8_t dlc)
{
    static const uint32_t table[9] = {
        FDCAN_DLC_BYTES_0, FDCAN_DLC_BYTES_1, FDCAN_DLC_BYTES_2, FDCAN_DLC_BYTES_3,
        FDCAN_DLC_BYTES_4, FDCAN_DLC_BYTES_5, FDCAN_DLC_BYTES_6, FDCAN_DLC_BYTES_7,
        FDCAN_DLC_BYTES_8,
    };
    return table[dlc > 8 ? 8 : dlc];
}

static uint8_t SendClassicFrame(uint32_t can_id, const uint8_t *data, uint8_t dlc)
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
    memcpy(padded, data, dlc);
    return HAL_FDCAN_AddMessageToTxFifoQ(&hfdcan1_app, &tx, padded) == HAL_OK;
}

/* Polls RxFifo0 for a real matching-ID response within timeout_ms - a
 * bounded busy-wait (this application has no RTOS task/queue plumbing
 * for FDCAN RX yet, matching the bootloader's own polling-not-interrupt
 * style for the exact same peripheral). A non-matching ID is silently
 * discarded and polling continues - real STACK A traffic includes every
 * OTHER slot's own heartbeats too, not just the one this call is waiting
 * on. */
static uint8_t WaitForResponse(uint32_t expected_can_id, uint8_t *out_data, uint8_t *out_dlc, uint32_t timeout_ms)
{
    uint32_t deadline = HAL_GetTick() + timeout_ms;
    while (HAL_GetTick() < deadline) {
        if (HAL_FDCAN_GetRxFifoFillLevel(&hfdcan1_app, FDCAN_RX_FIFO0) > 0) {
            FDCAN_RxHeaderTypeDef rx;
            uint8_t rx_data[8];
            if (HAL_FDCAN_GetRxMessage(&hfdcan1_app, FDCAN_RX_FIFO0, &rx, rx_data) == HAL_OK) {
                if (rx.Identifier == expected_can_id) {
                    uint8_t dlc = (uint8_t)(rx.DataLength >> 16); /* FDCAN_DLC_BYTES_n encodes the byte count in bits [19:16] - real HAL convention, matches DlcToFdcanDataLength()'s own table shape */
                    if (dlc > 8) dlc = 8;
                    memcpy(out_data, rx_data, dlc);
                    *out_dlc = dlc;
                    return 1;
                }
                /* not ours - a different slot's own traffic, keep waiting */
            }
        }
    }
    return 0;
}

uint8_t KinematicBrainCan_QueryAxisStatus(uint8_t slot, uint8_t out_status[8], uint8_t *out_dlc, uint32_t timeout_ms)
{
    uint32_t base = StackASlotBaseId(slot);
    if (!SendClassicFrame(base + OFS_AXIS_STATUS, NULL, 0)) return 0;
    return WaitForResponse(base + OFS_AXIS_STATUS, out_status, out_dlc, timeout_ms);
}

/* Real RELAY_SEND fragmentation - matches spi_bridge/relay_tunnel.py's own
 * build_relay_send_fragments() byte-for-byte: payload[0:2] = real target
 * CAN id (BE), payload[2] = real total dlc, payload[3:8] = up to 5 real
 * data bytes. A dlc>5 frame needs exactly 2 fragments. */
static void SendRelayFragments(uint8_t slot, uint16_t real_can_id, const uint8_t *data, uint8_t dlc)
{
    uint32_t relay_send_id = StackASlotBaseId(slot) + OFS_RELAY_SEND;
    uint8_t first_len = dlc <= RELAY_FRAGMENT_DATA_BYTES ? dlc : RELAY_FRAGMENT_DATA_BYTES;

    uint8_t fragment[8];
    fragment[0] = (uint8_t)(real_can_id >> 8);
    fragment[1] = (uint8_t)(real_can_id & 0xFF);
    fragment[2] = dlc;
    memcpy(&fragment[3], data, first_len);
    SendClassicFrame(relay_send_id, fragment, (uint8_t)(RELAY_FRAGMENT_HEADER_BYTES + first_len));

    if (dlc > RELAY_FRAGMENT_DATA_BYTES) {
        uint8_t second_len = dlc - RELAY_FRAGMENT_DATA_BYTES;
        fragment[0] = (uint8_t)(real_can_id >> 8);
        fragment[1] = (uint8_t)(real_can_id & 0xFF);
        fragment[2] = dlc; /* real total dlc repeated in both fragments - stateless per-transaction, matching the CM5 host side's own real design */
        memcpy(&fragment[3], data + RELAY_FRAGMENT_DATA_BYTES, second_len);
        SendClassicFrame(relay_send_id, fragment, (uint8_t)(RELAY_FRAGMENT_HEADER_BYTES + second_len));
    }
}

/* Polls RELAY_RECV, reassembling fragments for the first real CAN id it
 * sees until the real total dlc is accumulated, or times out - the exact
 * same real reassembly logic as spi_bridge/relay_tunnel.py's own
 * RelayedTransport._drain_relay_recv(), ported to C. */
static uint8_t DrainRelayRecv(uint8_t slot, uint8_t *out_data, uint8_t *out_dlc, uint32_t timeout_ms)
{
    uint32_t relay_recv_id = StackASlotBaseId(slot) + OFS_RELAY_RECV;
    uint32_t deadline = HAL_GetTick() + timeout_ms;
    uint8_t have_id = 0;
    uint8_t accumulated_dlc = 0;
    uint8_t accumulated_len = 0;
    uint8_t accumulated[8] = {0};

    while (HAL_GetTick() < deadline) {
        uint8_t response[8];
        uint8_t response_dlc = 0;
        /* RELAY_RECV is operator-polled (architecture.md section 5) - this
         * core sends an empty query frame on the real RELAY_RECV id each
         * poll to drain the Robot Controller Board's own FIFO one entry
         * at a time; a real Classic CAN request/response round trip, same
         * as AXIS_STATUS above. */
        if (!SendClassicFrame(relay_recv_id, NULL, 0)) continue;
        if (!WaitForResponse(relay_recv_id, response, &response_dlc, 50)) continue;
        if (response_dlc < RELAY_FRAGMENT_HEADER_BYTES) continue; /* nothing new queued this poll */

        uint16_t fragment_id = ((uint16_t)response[0] << 8) | response[1];
        uint8_t fragment_dlc = response[2];
        uint8_t fragment_len = (uint8_t)(response_dlc - RELAY_FRAGMENT_HEADER_BYTES);
        if (!have_id) {
            have_id = 1;
            accumulated_dlc = fragment_dlc;
            accumulated_len = 0;
        } else if (fragment_id != ((uint16_t)accumulated[0] << 8 | accumulated[1])) {
            continue; /* a different in-flight frame's fragment - not ours */
        }
        accumulated[0] = response[0];
        accumulated[1] = response[1];
        if (accumulated_len + fragment_len <= 8) {
            memcpy(out_data + accumulated_len, &response[RELAY_FRAGMENT_HEADER_BYTES], fragment_len);
            accumulated_len += fragment_len;
        }
        if (accumulated_len >= accumulated_dlc) {
            *out_dlc = accumulated_dlc;
            return 1;
        }
    }
    return 0;
}

uint8_t KinematicBrainCan_RelayToUrtcHead(
    uint8_t slot,
    uint16_t real_can_id,
    const uint8_t *data,
    uint8_t dlc,
    uint8_t wait_for_response,
    uint8_t out_data[8],
    uint8_t *out_dlc,
    uint32_t timeout_ms
) {
    SendRelayFragments(slot, real_can_id, data, dlc);
    if (!wait_for_response) return 1; /* fire-and-forget, matching bootloader_client.py's own _QUERY_FRAME_TYPES reasoning */
    return DrainRelayRecv(slot, out_data, out_dlc, timeout_ms);
}
