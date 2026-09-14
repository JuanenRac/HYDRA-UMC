/* =============================================================================
 * HYDRA-UMC - src/mcu_stm32g474/dlc_to_fdcan_data_length.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0 - see LICENSE
 * =============================================================================
 * A.11: the DLC (0-8, Classic CAN only - this board never sends CAN-FD)
 * to FDCAN peripheral data-length-code mapping, pulled out of
 * RobotControllerRelay.c's own private `static DlcToFdcanDataLength()`
 * into a HAL-free header so it can be regression-tested on the host with
 * plain gcc, no G474 board - same real motivation and pattern as
 * relay_rx_queue.h in this same directory.
 *
 * KinematicBrainCan.c (src/mcu_stm32h745/CM4/) carries a byte-identical
 * copy of this exact file for the same reason relay_rx_queue.h itself
 * documents at RobotControllerRelay.c's own call site: this project
 * builds each MCU target from its own isolated include path (see
 * build_firmware.sh), so a genuinely shared file needs a real per-target
 * copy, not a cross-directory #include. tests/test_dlc_to_fdcan_data_
 * length.c is compiled and run against BOTH copies, so a behavioral
 * drift between them - not just a textual one - is a real, real test
 * failure, not merely unnoticed. The 9 output values here match the STM32
 * FDCAN HAL's own FDCAN_DLC_BYTES_0..FDCAN_DLC_BYTES_8 (confirmed
 * identical across the G4 and H7 HAL headers before writing this) -
 * spelled out as literal integers rather than the HAL macro names
 * themselves, so this header never needs to include the HAL at all.
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
