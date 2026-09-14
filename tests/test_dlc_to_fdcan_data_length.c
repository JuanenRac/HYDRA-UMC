/* =============================================================================
 * HYDRA-UMC Firmware - Host logic tests for DlcToFdcanDataLength
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0 - see LICENSE
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

    if (failures == 0) {
        printf("  OK   all dlc_to_fdcan_data_length host tests passed\n");
        return 0;
    }
    printf("  %d assertion(s) failed\n", failures);
    return 1;
}
