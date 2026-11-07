/* =============================================================================
 * HYDRA-UMC Bootloader - adoption_guard.h: read-out protection and the adoption of an application (HAL-free, host-testable)
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see repo root LICENSE
 * =============================================================================
 * Two decisions of the bootloader of the controller board that the bench could only check on a protected chip, taken out so a host test can run them
 * with a simulated option byte:
 *
 *   1. The read-back of the installed application (HandleReadbackStart) streams the flash to any node of the bus. On a board in production
 *      (read-out protection level 1 or 2, option byte RDP != 0xAA) that stream would hand over, through the bootloader, what the protection keeps
 *      from the debug port. With the switch HYDRA_READBACK_GATE at 1 the read-back is refused whenever the protection is on. A wrong request is
 *      refused with no data at all: the answer to a refused request is a length of 0.
 *   2. The first-boot adoption of an application that has no metadata yet writes the metadata and trusts it. With the same switch the record is
 *      read back after the write and must match what was meant to be written (size, hardware identifier, CRC-32, SHA-256); a difference is a
 *      refusal to start the application, never a start from metadata that is not what the bootloader computed.
 *
 * The switch is 0 by default: the binary does not change. The option byte is read by the caller (FLASH->OPTR & 0xFF); on the host it is a number.
 * ========================================================================== */
#ifndef HYDRA_UMC_ADOPTION_GUARD_H
#define HYDRA_UMC_ADOPTION_GUARD_H

#include <stdint.h>
#include <string.h>

#ifndef HYDRA_READBACK_GATE
#define HYDRA_READBACK_GATE 0
#endif

#define ADOPT_RDP_LEVEL0_BYTE 0xAAu
#define ADOPT_RDP_LEVEL2_BYTE 0xCCu

/* 0, 1 or 2 for the option byte (tools/rdp_plan.py decodes it the same way). */
static inline uint8_t Adopt_RdpLevel(uint32_t optr)
{
    const uint8_t rdp = (uint8_t)(optr & 0xFFu);
    if (rdp == ADOPT_RDP_LEVEL0_BYTE) return 0u;
    return rdp == ADOPT_RDP_LEVEL2_BYTE ? 2u : 1u;
}

/* 1 when the read-back of the installed application may be served. With the switch off it always may (the behaviour of the earlier firmware). */
static inline uint8_t Adopt_ReadbackAllowed(uint32_t optr, uint8_t gate_enabled)
{
    return (uint8_t)(!gate_enabled || Adopt_RdpLevel(optr) == 0u);
}

/* The part of a metadata record the adoption computes. */
typedef struct {
    uint32_t size;
    uint32_t hardware_id;
    uint32_t crc32;
    uint8_t sha256[32];
} AdoptRecord_t;

/* 1 when the record read back is the record that was meant to be written. The digest is compared without an early exit. */
static inline uint8_t Adopt_RecordMatches(const AdoptRecord_t *wanted, const AdoptRecord_t *read_back)
{
    uint8_t difference = (uint8_t)((wanted->size != read_back->size) | (wanted->hardware_id != read_back->hardware_id) | (wanted->crc32 != read_back->crc32));
    for (int i = 0; i < 32; i++) difference |= (uint8_t)(wanted->sha256[i] ^ read_back->sha256[i]);
    return (uint8_t)(difference == 0u);
}

#endif /* HYDRA_UMC_ADOPTION_GUARD_H */
