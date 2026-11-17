/* =============================================================================
 * HYDRA-UMC Firmware - host logic test for readout_gate.h of the H7 bootloaders (CM7 and CM4)
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * The decision that HandleReadbackStart takes from the read-out protection byte (bits 15:8 of FLASH->OPTSR_CUR), run with simulated option words.
 * Built in both bootloader folders and with HYDRA_READBACK_GATE at 0 and at 1 (the default is 0). Evidence: simulator; nothing runs on a board.
 * ========================================================================== */
#include <stdio.h>

#include "readout_gate.h"

static int failures;

#define TEST_ASSERT(cond, msg) \
    do { if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } } while (0)

int main(void)
{
    printf("HYDRA-UMC host logic tests - readout_gate.h (H7), gate %d\n", HYDRA_READBACK_GATE);
    TEST_ASSERT(H7_RdpLevel(0x0000AA00u) == 0u, "byte 0xAA in bits 15:8 is level 0");
    TEST_ASSERT(H7_RdpLevel(0xFFFFAAFFu) == 0u, "the other bits of the word do not matter");
    TEST_ASSERT(H7_RdpLevel(0x0000CC00u) == 2u, "byte 0xCC is level 2");
    TEST_ASSERT(H7_RdpLevel(0x0000BB00u) == 1u, "any other value is level 1");
    TEST_ASSERT(H7_RdpLevel(0x000000AAu) == 1u, "0xAA in the low byte is not the protection byte");
    TEST_ASSERT(H7_RdpLevel(0x00000000u) == 1u, "an all-zero word is protected");
    TEST_ASSERT(H7_ReadbackAllowed(0x0000BB00u, 0u) == 1u, "with the gate off a protected board is served (the earlier behaviour)");
    TEST_ASSERT(H7_ReadbackAllowed(0x0000AA00u, 1u) == 1u, "with the gate on, level 0 is served");
    TEST_ASSERT(H7_ReadbackAllowed(0x0000BB00u, 1u) == 0u, "with the gate on, level 1 is refused");
    TEST_ASSERT(H7_ReadbackAllowed(0x0000CC00u, 1u) == 0u, "with the gate on, level 2 is refused");
    TEST_ASSERT(H7_ReadbackAllowed(0x00000000u, 1u) == 0u, "with the gate on, an all-zero word is refused");
    TEST_ASSERT(HYDRA_READBACK_GATE == 0 || HYDRA_READBACK_GATE == 1, "the switch is 0 or 1");
    if (failures) {
        printf("RESULT: FAIL (%d)\n", failures);
        return 1;
    }
    printf("RESULT: PASS\n");
    return 0;
}
