/* =============================================================================
 * HYDRA-UMC Firmware - Host logic tests for DlcToFdcanDataLength
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * A.11: locks in the DLC (0-8, Classic CAN) -> FDCAN data-length-code
 * mapping both RobotControllerRelay.c (src/mcu_stm32g474/) and
 * KinematicBrainCan.c (src/mcu_stm32h745/CM4/) build their TX headers
 * from. This exact source file is compiled TWICE by build_firmware.sh's
 * own host-tests step - once per target's own include path - so it
 * proves BOTH real per-target copies of dlc_to_fdcan_data_length.h
 * behave identically, not just one of them.
 */
#include <stdio.h>

#include "dlc_to_fdcan_data_length.h"

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("  FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
            failures++; \
        } \
    } while (0)

int main(void)
{
    int failures = 0;
    printf("HYDRA-UMC host logic tests - dlc_to_fdcan_data_length.h\n");

    /* Every real Classic CAN DLC (0-8) maps to itself as a byte count. */
    for (uint8_t dlc = 0; dlc <= 8; dlc++) {
        TEST_ASSERT(DlcToFdcanDataLength(dlc) == dlc, "DLC 0-8 maps to itself");
    }

    /* A DLC beyond this board's real Classic CAN range clamps to 8 bytes,
     * the same real safety margin RelayRxQueue_Push's own dlc clamp uses -
     * never reads/writes past the fixed 8-byte data[] a real frame has. */
    TEST_ASSERT(DlcToFdcanDataLength(9) == 8, "DLC 9 clamps to 8 bytes");
    TEST_ASSERT(DlcToFdcanDataLength(15) == 8, "DLC 15 clamps to 8 bytes");
    TEST_ASSERT(DlcToFdcanDataLength(255) == 8, "DLC 255 (garbage) clamps to 8 bytes");

    /* Receive side: the HAL hands back the plain 0..15 DLC code, so 0-8
     * pass through and anything above clamps to the 8-byte classic frame. */
    for (uint32_t code = 0; code <= 8; code++) {
        TEST_ASSERT(FdcanDataLengthToDlc(code) == code, "RX code 0-8 maps to itself");
    }
    TEST_ASSERT(FdcanDataLengthToDlc(9) == 8, "RX code 9 clamps to 8");
    TEST_ASSERT(FdcanDataLengthToDlc(15) == 8, "RX code 15 clamps to 8");

    /* Mini-HAL round trip with the real semantics (HAL_FDCAN_AddMessageToTxFifoQ
     * writes DataLength << 16 into the message RAM word and
     * HAL_FDCAN_GetRxMessage reads it back with >> 16): what one side sends
     * the other side must read back unchanged. A TX that pre-shifted its
     * value (the old bootloader macro) would land in the wrong bits and an
     * RX that shifted again would always read 0. */
    for (uint8_t dlc = 0; dlc <= 8; dlc++) {
        uint32_t message_ram_word = DlcToFdcanDataLength(dlc) << 16;
        uint32_t rx_data_length = message_ram_word >> 16;
        TEST_ASSERT(FdcanDataLengthToDlc(rx_data_length) == dlc, "TX -> message RAM -> RX round trip keeps the DLC");
    }

    if (failures == 0) {
        printf("  OK   all dlc_to_fdcan_data_length host tests passed\n");
        return 0;
    }
    printf("  %d assertion(s) failed\n", failures);
    return 1;
}
