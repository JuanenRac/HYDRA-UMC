/* =============================================================================
 * HYDRA-UMC Bootloader - flash_layer.h: one flash layer for the three bootloaders (HAL-free, host-testable)
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see repo root LICENSE
 * =============================================================================
 * The three bootloaders (controller board G474, the CM4 and the CM7 of the H745) each had their own erase, program and verify code, with the same
 * checks written three times. This layer holds the checks once and leaves only the three hardware calls (erase, program, read) to each chip:
 *
 *   - the geometry says where the flash is and in which units it is erased and programmed (the G474: pages of 2 KB, double words of 8 bytes; the
 *     H745: sectors of 128 KB, flash words of 32 bytes);
 *   - every range is checked against the geometry before a call reaches the hardware (inside the flash, aligned to the unit, no wrap-around);
 *   - a write is padded to the program unit with 0xFF, written, and read back; a difference is a failure, not a silent bad image;
 *   - a copy goes through a small buffer and checks every block after it is written.
 *
 * The same file lives in the three boot directories (tests/test_flash_layer_parity.py fails when the copies differ). The bootloaders do not call it
 * yet: it is switched in chip by chip after the host test (tests/test_flash_layer.c, which runs both geometries on a simulated flash with a cut of
 * the power at every program unit) and bench rows 22 to 25 passed on the boards.
 * ========================================================================== */
#ifndef HYDRA_UMC_FLASH_LAYER_H
#define HYDRA_UMC_FLASH_LAYER_H

#include <stdint.h>
#include <string.h>

typedef struct {
    uint32_t base;          /* first address of the flash */
    uint32_t size;          /* bytes of flash */
    uint32_t erase_unit;    /* smallest erasable block */
    uint32_t program_unit;  /* smallest programmable block; addresses and lengths of a write are multiples of it */
} FlashLayerGeometry_t;

typedef enum {
    FLASH_LAYER_OK = 0,
    FLASH_LAYER_BAD_RANGE,      /* outside the flash, wrapping, or zero length */
    FLASH_LAYER_UNALIGNED,      /* address or length not a multiple of the unit */
    FLASH_LAYER_ERASE_FAILED,
    FLASH_LAYER_PROGRAM_FAILED,
    FLASH_LAYER_READ_FAILED,
    FLASH_LAYER_VERIFY_FAILED,  /* read-back differs from what was written */
    FLASH_LAYER_NOT_BLANK       /* a write to a place that is not erased */
} FlashLayerStatus;

typedef struct {
    const FlashLayerGeometry_t *geometry;
    void *context;
    int (*erase)(void *context, uint32_t address, uint32_t length);                          /* 0 on success */
    int (*program)(void *context, uint32_t address, const uint8_t *data, uint32_t length);   /* 0 on success */
    int (*read)(void *context, uint32_t address, uint8_t *out, uint32_t length);             /* 0 on success */
} FlashLayerOps_t;

#define FLASH_LAYER_MAX_PROGRAM_UNIT 32u
#define FLASH_LAYER_COPY_BLOCK       256u

static inline const FlashLayerGeometry_t *FlashLayer_GeometryG474(void)
{
    static const FlashLayerGeometry_t g = {0x08000000u, 0x00080000u, 2048u, 8u};
    return &g;
}

static inline const FlashLayerGeometry_t *FlashLayer_GeometryH745(void)
{
    static const FlashLayerGeometry_t g = {0x08000000u, 0x00200000u, 0x00020000u, 32u};
    return &g;
}

/* 1 when [address, address + length) is inside the flash and does not wrap. */
static inline uint8_t FlashLayer_InRange(const FlashLayerGeometry_t *g, uint32_t address, uint32_t length)
{
    if (length == 0u || address < g->base) return 0u;
    const uint32_t offset = address - g->base;
    return (uint8_t)(offset < g->size && length <= g->size - offset);
}

static inline FlashLayerStatus FlashLayer_Check(const FlashLayerGeometry_t *g, uint32_t address, uint32_t length, uint32_t unit)
{
    if (!FlashLayer_InRange(g, address, length)) return FLASH_LAYER_BAD_RANGE;
    if (unit == 0u || (address - g->base) % unit != 0u || length % unit != 0u) return FLASH_LAYER_UNALIGNED;
    return FLASH_LAYER_OK;
}

static inline FlashLayerStatus FlashLayer_Erase(const FlashLayerOps_t *ops, uint32_t address, uint32_t length)
{
    const FlashLayerStatus check = FlashLayer_Check(ops->geometry, address, length, ops->geometry->erase_unit);
    if (check != FLASH_LAYER_OK) return check;
    return ops->erase(ops->context, address, length) == 0 ? FLASH_LAYER_OK : FLASH_LAYER_ERASE_FAILED;
}

/* 1 when every byte of the range reads 0xFF. */
static inline FlashLayerStatus FlashLayer_IsBlank(const FlashLayerOps_t *ops, uint32_t address, uint32_t length, uint8_t *blank)
{
    uint8_t block[FLASH_LAYER_MAX_PROGRAM_UNIT];
    const FlashLayerGeometry_t *g = ops->geometry;
    if (g->program_unit > FLASH_LAYER_MAX_PROGRAM_UNIT) return FLASH_LAYER_UNALIGNED;
    const FlashLayerStatus check = FlashLayer_Check(g, address, length, g->program_unit);
    if (check != FLASH_LAYER_OK) return check;
    *blank = 1u;
    for (uint32_t done = 0u; done < length; done += g->program_unit) {
        if (ops->read(ops->context, address + done, block, g->program_unit) != 0) return FLASH_LAYER_READ_FAILED;
        for (uint32_t i = 0u; i < g->program_unit; i++) {
            if (block[i] != 0xFFu) { *blank = 0u; return FLASH_LAYER_OK; }
        }
    }
    return FLASH_LAYER_OK;
}

/* Writes `length` bytes at `address` (a multiple of the program unit). The last partial unit is padded with 0xFF. The place must be erased. Every
 * unit is read back after it is programmed. */
static inline FlashLayerStatus FlashLayer_Write(const FlashLayerOps_t *ops, uint32_t address, const uint8_t *data, uint32_t length)
{
    const FlashLayerGeometry_t *g = ops->geometry;
    uint8_t unit[FLASH_LAYER_MAX_PROGRAM_UNIT];
    uint8_t back[FLASH_LAYER_MAX_PROGRAM_UNIT];
    if (g->program_unit == 0u || g->program_unit > FLASH_LAYER_MAX_PROGRAM_UNIT) return FLASH_LAYER_UNALIGNED;
    const uint32_t padded = ((length + g->program_unit - 1u) / g->program_unit) * g->program_unit;
    if (length == 0u) return FLASH_LAYER_BAD_RANGE;
    FlashLayerStatus check = FlashLayer_Check(g, address, padded, g->program_unit);
    if (check != FLASH_LAYER_OK) return check;
    uint8_t blank = 0u;
    check = FlashLayer_IsBlank(ops, address, padded, &blank);
    if (check != FLASH_LAYER_OK) return check;
    if (!blank) return FLASH_LAYER_NOT_BLANK;
    for (uint32_t done = 0u; done < padded; done += g->program_unit) {
        const uint32_t left = length > done ? length - done : 0u;
        const uint32_t take = left < g->program_unit ? left : g->program_unit;
        memset(unit, 0xFF, g->program_unit);
        if (take > 0u) memcpy(unit, data + done, take);
        if (ops->program(ops->context, address + done, unit, g->program_unit) != 0) return FLASH_LAYER_PROGRAM_FAILED;
        if (ops->read(ops->context, address + done, back, g->program_unit) != 0) return FLASH_LAYER_READ_FAILED;
        if (memcmp(unit, back, g->program_unit) != 0) return FLASH_LAYER_VERIFY_FAILED;
    }
    return FLASH_LAYER_OK;
}

/* Erases the destination and copies `length` bytes (a multiple of the program unit) from `source` to `destination` in blocks, checking each block. */
static inline FlashLayerStatus FlashLayer_Copy(const FlashLayerOps_t *ops, uint32_t destination, uint32_t source, uint32_t length)
{
    const FlashLayerGeometry_t *g = ops->geometry;
    uint8_t block[FLASH_LAYER_COPY_BLOCK];
    FlashLayerStatus check = FlashLayer_Check(g, source, length, g->program_unit);
    if (check != FLASH_LAYER_OK) return check;
    check = FlashLayer_Check(g, destination, length, g->program_unit);
    if (check != FLASH_LAYER_OK) return check;
    const uint32_t erase_length = ((length + g->erase_unit - 1u) / g->erase_unit) * g->erase_unit;
    check = FlashLayer_Erase(ops, destination, erase_length);
    if (check != FLASH_LAYER_OK) return check;
    for (uint32_t done = 0u; done < length;) {
        uint32_t take = length - done;
        if (take > FLASH_LAYER_COPY_BLOCK) take = FLASH_LAYER_COPY_BLOCK;
        take -= take % g->program_unit;
        if (take == 0u) return FLASH_LAYER_UNALIGNED;
        if (ops->read(ops->context, source + done, block, take) != 0) return FLASH_LAYER_READ_FAILED;
        check = FlashLayer_Write(ops, destination + done, block, take);
        if (check != FLASH_LAYER_OK) return check;
        done += take;
    }
    return FLASH_LAYER_OK;
}

#endif /* HYDRA_UMC_FLASH_LAYER_H */
