/* =============================================================================
 * HYDRA-UMC Bootloader (STM32H745/H757) - readout_gate.h: read-out protection and the read-back of the installed application (HAL-free, host-testable)
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see repo root LICENSE
 * =============================================================================
 * The read-back of the installed application (HandleReadbackStart) hands the flash to the node that asks for it. On a board in production (read-out
 * protection level 1 or 2) that would hand over, through the bootloader, what the protection keeps from the debug port. With the switch
 * HYDRA_READBACK_GATE at 1 the read-back is refused whenever the protection is on: the answer to a refused request is a length of 0 and no data.
 *
 * The switch is 0 by default: the binary does not change. The option word is read by the caller (FLASH->OPTSR_CUR); on the host it is a number.
 * The protection level is the byte RDP in bits 15:8 of that word: 0xAA is level 0 (off), 0xCC is level 2 (permanent), anything else is level 1.
 * Same decision as the controller board (src/mcu_stm32g474/boot/adoption_guard.h), whose option byte is the low byte of FLASH->OPTR.
 * This file is identical in the CM7 and CM4 bootloader folders.
 * ========================================================================== */
#ifndef HYDRA_UMC_H7_READOUT_GATE_H
#define HYDRA_UMC_H7_READOUT_GATE_H

#include <stdint.h>

#ifndef HYDRA_READBACK_GATE
#define HYDRA_READBACK_GATE 0
#endif

#define H7_RDP_LEVEL0_BYTE 0xAAu
#define H7_RDP_LEVEL2_BYTE 0xCCu

/* 0, 1 or 2 for the option word. */
static inline uint8_t H7_RdpLevel(uint32_t optsr)
{
    const uint8_t rdp = (uint8_t)((optsr >> 8) & 0xFFu);
    if (rdp == H7_RDP_LEVEL0_BYTE) return 0u;
    return rdp == H7_RDP_LEVEL2_BYTE ? 2u : 1u;
}

/* 1 when the read-back of the installed application may be served. With the switch off it always may (the behaviour of the earlier firmware). */
static inline uint8_t H7_ReadbackAllowed(uint32_t optsr, uint8_t gate_enabled)
{
    return (uint8_t)(!gate_enabled || H7_RdpLevel(optsr) == 0u);
}

#endif /* HYDRA_UMC_H7_READOUT_GATE_H */
