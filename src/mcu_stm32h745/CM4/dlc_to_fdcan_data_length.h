/* =============================================================================
 * HYDRA-UMC - src/mcu_stm32h745/CM4/dlc_to_fdcan_data_length.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0 - see LICENSE
 * =============================================================================
 * A.11: the DLC (0-8, Classic CAN only - this core never sends CAN-FD) to
 * FDCAN peripheral data-length-code mapping, pulled out of
 * KinematicBrainCan.c's own private `static DlcToFdcanDataLength()` into
 * a HAL-free header so it can be regression-tested on the host with
 * plain gcc, no H745 board.
 *
 * RobotControllerRelay.c (src/mcu_stm32g474/) carries a byte-identical
 * copy of this exact file - see that copy's own header comment for why
 * (this project builds each MCU target from its own isolated include
 * path, so a genuinely shared file needs a real per-target copy, not a
 * cross-directory #include). tests/test_dlc_to_fdcan_data_length.c is
 * compiled and run against BOTH copies, so a behavioral drift between
 * them is a real test failure, not merely unnoticed.
 */
#ifndef HYDRA_UMC_DLC_TO_FDCAN_DATA_LENGTH_H
#define HYDRA_UMC_DLC_TO_FDCAN_DATA_LENGTH_H

#include <stdint.h>

static inline uint32_t DlcToFdcanDataLength(uint8_t dlc)
{
    static const uint32_t table[9] = {0, 1, 2, 3, 4, 5, 6, 7, 8};
    return table[dlc > 8 ? 8 : dlc];
}

#endif /* HYDRA_UMC_DLC_TO_FDCAN_DATA_LENGTH_H */
