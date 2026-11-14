/* =============================================================================
 * HYDRA-UMC Firmware - tests/test_gateway_fdcan1.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Host test of the FDCAN1 binding of the stack tier of the gateway (src/mcu_stm32h757/CM7/GatewayFdcan1.c) against a double of the HAL (tests/stubs/h7fdcan): the double plays the
 * transmit FIFO and RX FIFO0 of the peripheral and a tick that moves with every look. It checks the header of a frame that goes out (standard id, data frame, Classic CAN), a full
 * FIFO and a refused frame, the frames that are ignored when they come in (extended id, remote frame, a read that fails), the clamp of a damaged length, the watchdog hook, and the
 * refusal of a missing peripheral or of a time-out out of range. Evidence: simulator.
 *
 *   cc -std=c11 -Wall -Wextra -DHYDRA_GATEWAY_FDCAN1=1 -Itests/stubs/h7fdcan -Isrc/mcu_stm32h757/CM7 -Isrc/mcu_stm32h757/Common \
 *      -o test_gateway_fdcan1 tests/test_gateway_fdcan1.c src/mcu_stm32h757/CM7/GatewayFdcan1.c
 */
#include <stdio.h>
#include <string.h>

#include "GatewayFdcan1.h"

#if !HYDRA_GATEWAY_FDCAN1
#error "this test is for the binding with its switch on: build it with -DHYDRA_GATEWAY_FDCAN1=1"
#endif

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } } while (0)

typedef struct {
    uint32_t id, id_type, frame_type, dlc;
    uint8_t data[8];
} Rx;

static uint32_t tx_free = 3u, tx_adds, tx_fail, tick;
static FDCAN_TxHeaderTypeDef tx_seen;
static uint8_t tx_data_seen[8];
static Rx rx_queue[4];
static unsigned rx_count, rx_read;
static int rx_get_fails;
static unsigned kicks;

uint32_t HAL_GetTick(void) { return tick++; }
uint32_t HAL_FDCAN_GetTxFifoFreeLevel(FDCAN_HandleTypeDef *h) { (void)h; return tx_free; }

HAL_StatusTypeDef HAL_FDCAN_AddMessageToTxFifoQ(FDCAN_HandleTypeDef *h, FDCAN_TxHeaderTypeDef *header, uint8_t *data)
{
    (void)h;
    tx_adds++;
    tx_seen = *header;
    memcpy(tx_data_seen, data, 8);
    return tx_fail ? HAL_ERROR : HAL_OK;
}

uint32_t HAL_FDCAN_GetRxFifoFillLevel(FDCAN_HandleTypeDef *h, uint32_t fifo)
{
    (void)h;
    return fifo == FDCAN_RX_FIFO0 ? (rx_count - rx_read) : 0u;
}

HAL_StatusTypeDef HAL_FDCAN_GetRxMessage(FDCAN_HandleTypeDef *h, uint32_t fifo, FDCAN_RxHeaderTypeDef *header, uint8_t *data)
{
    (void)h; (void)fifo;
    const Rx *r = &rx_queue[rx_read++];
    if (rx_get_fails) return HAL_ERROR;
    memset(header, 0, sizeof *header);
    header->Identifier = r->id;
    header->IdType = r->id_type;
    header->RxFrameType = r->frame_type;
    header->DataLength = r->dlc;
    memcpy(data, r->data, 8);
    return HAL_OK;
}

static void kick(void) { kicks++; }

static void queue(uint32_t id, uint32_t id_type, uint32_t frame_type, uint32_t dlc, uint8_t first)
{
    Rx *r = &rx_queue[rx_count++];
    r->id = id; r->id_type = id_type; r->frame_type = frame_type; r->dlc = dlc;
    for (unsigned i = 0; i < 8u; i++) r->data[i] = (uint8_t)(first + i);
}

static void reset(void)
{
    tx_free = 3u; tx_adds = 0u; tx_fail = 0u; tick = 0u;
    rx_count = 0u; rx_read = 0u; rx_get_fails = 0; kicks = 0u;
    memset(&tx_seen, 0xAA, sizeof tx_seen);
}

int main(void)
{
    FDCAN_GlobalTypeDef instance = {1};
    FDCAN_HandleTypeDef handle = {&instance};
    GatewayReply reply;
    const uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};

    CHECK(GatewayFdcan1_Bind(NULL, 200u, NULL) == NULL, "no peripheral");
    CHECK(GatewayFdcan1_Bind(&handle, 0u, NULL) == NULL, "time-out 0");
    CHECK(GatewayFdcan1_Bind(&handle, 1001u, NULL) == NULL, "time-out over 1000");

    reset();
    GatewayStackA_t *stack = GatewayFdcan1_Bind(&handle, 20u, kick);
    CHECK(stack != NULL, "a good binding");
    queue(0x600UL + 0x40UL + 0x05UL, FDCAN_STANDARD_ID, FDCAN_DATA_FRAME, 1u, 0x30);
    CHECK(GatewayStackA_Exchange(stack, 1u, 0x01u, payload, 4u, &reply) == 1, "an exchange through the peripheral");
    CHECK(tx_adds == 1u && tx_seen.Identifier == 0x640UL + 0x01UL && tx_seen.IdType == FDCAN_STANDARD_ID && tx_seen.TxFrameType == FDCAN_DATA_FRAME, "the frame went out with a standard id");
    CHECK(tx_seen.DataLength == 4u && tx_seen.FDFormat == FDCAN_CLASSIC_CAN && tx_seen.BitRateSwitch == FDCAN_BRS_OFF && tx_seen.ErrorStateIndicator == FDCAN_ESI_ACTIVE, "Classic CAN with the length of the request");
    CHECK(tx_seen.TxEventFifoControl == FDCAN_NO_TX_EVENTS && tx_seen.MessageMarker == 0u && tx_data_seen[3] == 4u, "no events, payload copied");
    CHECK(reply.type == 0x05u && reply.dlc == 1u && reply.payload[0] == 0x30u && kicks > 0u, "the answer and the watchdog hook");

    /* frames that are ignored, and a length that is damaged */
    reset();
    queue(0x640UL + 0x06UL, FDCAN_EXTENDED_ID, FDCAN_DATA_FRAME, 2u, 1);   /* extended id */
    queue(0x640UL + 0x06UL, FDCAN_STANDARD_ID, FDCAN_REMOTE_FRAME, 2u, 2); /* remote frame */
    queue(0x640UL + 0x07UL, FDCAN_STANDARD_ID, FDCAN_DATA_FRAME, 12u, 3);  /* a length over 8 */
    CHECK(GatewayStackA_Exchange(stack, 1u, 0x01u, payload, 1u, &reply) == 1 && reply.type == 0x07u && reply.dlc == 8u, "ignored frames are skipped and a length over 8 is clamped");

    /* a read that fails is no frame */
    reset();
    queue(0x640UL + 0x05UL, FDCAN_STANDARD_ID, FDCAN_DATA_FRAME, 1u, 1);
    rx_get_fails = 1;
    CHECK(GatewayStackA_Exchange(stack, 1u, 0x01u, payload, 1u, &reply) == 0, "a failing read ends in a time-out");

    /* a full transmit FIFO, a refused frame */
    reset();
    tx_free = 0u;
    CHECK(GatewayStackA_Exchange(stack, 1u, 0x01u, payload, 1u, &reply) == 0 && tx_adds == 0u, "a full FIFO is an error and nothing is queued");
    reset();
    tx_fail = 1u;
    CHECK(GatewayStackA_Exchange(stack, 1u, 0x01u, payload, 1u, &reply) == 0 && tx_adds == 1u, "a refused frame is an error");

    /* a length over 8 in the request goes out as 8 (the callback already clamps it) and a binding without a hook still waits */
    reset();
    stack = GatewayFdcan1_Bind(&handle, 5u, NULL);
    CHECK(stack != NULL, "a binding without a hook");
    CHECK(GatewayStackA_Exchange(stack, 0u, 0x02u, payload, 8u, &reply) == 0 && kicks == 0u && tx_seen.DataLength == 8u, "silence ends in a time-out without a hook");

    if (failures) {
        printf("gateway_fdcan1 host tests: %d FAILED\n", failures);
        return 1;
    }
    printf("gateway_fdcan1 host logic tests passed\n");
    return 0;
}
