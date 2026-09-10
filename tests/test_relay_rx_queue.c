/* =============================================================================
 * HYDRA-UMC Firmware - Host logic tests for the Robot Controller Board's
 * FDCAN2 capture ring buffer (src/mcu_stm32g474/relay_rx_queue.h)
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0 - see LICENSE
 * =============================================================================
 * Locks in the ring's contract: FIFO order, wrap-around past the fixed depth,
 * and the "drop the OLDEST frame when full" policy (never the newest) that the
 * RELAY_RECV drain path on this board depends on. HAL-free - runs on host gcc.
 */
#include <string.h>

#include "test_runner.h"
#include "mcu_stm32g474/relay_rx_queue.h"

static RelayCapturedFrame_t mk(uint16_t id, uint8_t dlc)
{
    RelayCapturedFrame_t f;
    memset(&f, 0, sizeof(f));
    f.can_id = id;
    f.dlc = dlc;
    for (uint8_t i = 0; i < 8; i++) f.data[i] = (uint8_t)(id + i);
    return f;
}

void run_relay_rx_queue_tests(int *failures)
{
    RelayRxQueue_t q;
    RelayCapturedFrame_t out;

    /* 1. Reset gives an empty queue; pop from empty returns 0. */
    RelayRxQueue_Reset(&q);
    TEST_ASSERT(q.count == 0, "Reset -> count 0");
    TEST_ASSERT(RelayRxQueue_Pop(&q, &out) == 0, "Pop on empty returns 0");

    /* 2. FIFO order + payload integrity for a partial fill. */
    for (uint16_t i = 1; i <= 3; i++) {
        RelayCapturedFrame_t f = mk(0x100 + i, 4);
        RelayRxQueue_Push(&q, f.can_id, f.data, f.dlc);
    }
    TEST_ASSERT(q.count == 3, "count 3 after 3 pushes");
    for (uint16_t i = 1; i <= 3; i++) {
        TEST_ASSERT(RelayRxQueue_Pop(&q, &out) == 1, "Pop returns 1 while non-empty");
        TEST_ASSERT(out.can_id == 0x100 + i, "FIFO order preserved");
        TEST_ASSERT(out.dlc == 4 && out.data[0] == (uint8_t)(0x100 + i), "payload intact");
    }
    TEST_ASSERT(q.count == 0, "count 0 after draining");

    /* 3. Wrap-around: many single push/pop cycles across the fixed depth. */
    for (uint16_t i = 0; i < 100; i++) {
        RelayCapturedFrame_t f = mk((uint16_t)(0x200 + i), 8);
        RelayRxQueue_Push(&q, f.can_id, f.data, f.dlc);
        TEST_ASSERT(RelayRxQueue_Pop(&q, &out) == 1, "wrap: pop ok");
        TEST_ASSERT(out.can_id == (uint16_t)(0x200 + i), "wrap: correct frame across the ring boundary");
    }
    TEST_ASSERT(q.count == 0, "wrap: empty at the end");

    /* 4. Overflow drops the OLDEST, keeps the DEPTH newest, count never exceeds DEPTH. */
    RelayRxQueue_Reset(&q);
    for (uint16_t i = 1; i <= RELAY_RX_QUEUE_DEPTH + 3; i++) {
        RelayCapturedFrame_t f = mk((uint16_t)(0x300 + i), 2);
        RelayRxQueue_Push(&q, f.can_id, f.data, f.dlc);
        TEST_ASSERT(q.count <= RELAY_RX_QUEUE_DEPTH, "count never exceeds DEPTH");
    }
    TEST_ASSERT(q.count == RELAY_RX_QUEUE_DEPTH, "full queue holds exactly DEPTH");
    /* the 3 oldest (0x301..0x303) were dropped; first pop is 0x304 */
    for (uint16_t i = 4; i <= RELAY_RX_QUEUE_DEPTH + 3; i++) {
        TEST_ASSERT(RelayRxQueue_Pop(&q, &out) == 1, "overflow: pop ok");
        TEST_ASSERT(out.can_id == (uint16_t)(0x300 + i),
                    "overflow dropped the oldest, not the newest");
    }
    TEST_ASSERT(q.count == 0, "overflow: drained clean");

    /* 5. A DLC over 8 is clamped to 8 on the way in; nothing past 8 is copied. */
    RelayRxQueue_Reset(&q);
    uint8_t big[8] = {11, 22, 33, 44, 55, 66, 77, 88};
    RelayRxQueue_Push(&q, 0x7AB, big, 99);
    TEST_ASSERT(RelayRxQueue_Pop(&q, &out) == 1, "clamp: pop ok");
    TEST_ASSERT(out.dlc == 8, "DLC > 8 clamped to 8");
    TEST_ASSERT(out.data[7] == 88, "all 8 real bytes copied");
}

int main(void)
{
    int failures = 0;
    printf("HYDRA-UMC host logic tests - relay_rx_queue.h\n");
    run_relay_rx_queue_tests(&failures);
    if (failures == 0) {
        printf("  OK   all relay_rx_queue host tests passed\n");
        return 0;
    }
    printf("  %d assertion(s) failed\n", failures);
    return 1;
}
