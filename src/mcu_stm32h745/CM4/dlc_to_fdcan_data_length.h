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

/* Inverse of the above, for a received frame: the HAL hands back
 * FDCAN_RxHeaderTypeDef.DataLength as the raw 4-bit DLC field (0..15) -
 * NOT shifted into bits [19:16]; HAL_FDCAN_GetRxMessage() already did the
 * shift. This board only runs Classic CAN, so anything above 8 clamps to
 * 8 (the size of the fixed data[] a frame has here). */
static inline uint32_t FdcanDataLengthToDlc(uint32_t hal_data_length)
{
    return hal_data_length > 8U ? 8U : hal_data_length;
}

#endif /* HYDRA_UMC_DLC_TO_FDCAN_DATA_LENGTH_H */
