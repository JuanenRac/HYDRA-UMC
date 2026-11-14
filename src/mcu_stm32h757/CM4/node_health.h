/* =============================================================================
 * HYDRA-UMC - src/mcu_stm32h757/CM4/node_health.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * The last health frame of each board of the stack (HAL-free, host-testable). A controller board built with HYDRA_HEALTH_FRAME=1 sends one 8-byte frame per
 * second on slot_base + 0x11 (src/mcu_stm32g474/health_frame.h, tools/health_frame.py):
 *
 *   marker 0x48 | sequence | flags | tx errors | rx errors | stalls | uptime in units of 16 s | check (0xA5 xor the seven bytes before it)
 *
 * The brain keeps the last valid frame of every slot and the time it was heard, so the CM5 can ask for it over the SPI link (type 0x27, answered with 0x2A,
 * docs/SPI_SYSTEM.md). A frame with the wrong length, marker or check byte is not stored: the offset 0x11 is also the one of the axis step telemetry, which
 * is not a health frame and must not be taken for one. Everything here is read only for the stack; nothing in this file sends a frame.
 *
 * One writer (the code that reads the CAN bus, the STACK A task) and one reader (the same task, which copies the frames to the SPI service). The switch
 * HYDRA_SPI_HEALTH (0 by default) decides in KinematicBrainCan.c whether any of this is used.
 * ========================================================================== */
#ifndef HYDRA_UMC_NODE_HEALTH_H
#define HYDRA_UMC_NODE_HEALTH_H

#include <stdint.h>
#include <string.h>

#ifndef HYDRA_SPI_HEALTH
#define HYDRA_SPI_HEALTH 0 /* the same default as spi_system.h: the health of the boards is not kept or offered until a build asks for it */
#endif

#define NODE_HEALTH_SLOTS  8u
#define NODE_HEALTH_MARKER 0x48u
#define NODE_HEALTH_OFFSET 0x11u /* from the base identifier of the slot */
#define NODE_HEALTH_TICK_MS 100u /* the unit of the age that goes over the SPI link */

typedef struct {
    uint8_t seen[NODE_HEALTH_SLOTS];
    uint8_t frame[NODE_HEALTH_SLOTS][8];
    uint32_t heard_ms[NODE_HEALTH_SLOTS];
} NodeHealth_t;

static inline void NodeHealth_Init(NodeHealth_t *store)
{
    memset(store, 0, sizeof *store);
}

static inline uint8_t NodeHealth_Check(const uint8_t frame[8])
{
    uint8_t value = 0xA5u;
    for (int index = 0; index < 7; index++) value = (uint8_t)(value ^ frame[index]);
    return value;
}

/* 1 when `data` is a health frame of the slot (8 bytes, marker, check byte) and was stored. */
static inline uint8_t NodeHealth_OnFrame(NodeHealth_t *store, uint8_t slot, const uint8_t *data, uint8_t dlc, uint32_t now_ms)
{
    if (slot >= NODE_HEALTH_SLOTS || dlc != 8u || data[0] != NODE_HEALTH_MARKER || data[7] != NodeHealth_Check(data)) return 0u;
    memcpy(store->frame[slot], data, 8);
    store->heard_ms[slot] = now_ms;
    store->seen[slot] = 1u;
    return 1u;
}

/* Units of NODE_HEALTH_TICK_MS since the frame of the slot was heard, saturated at 254 (the wrapping clock is handled by the unsigned difference). */
static inline uint8_t NodeHealth_Age(const NodeHealth_t *store, uint8_t slot, uint32_t now_ms)
{
    const uint32_t ticks = (uint32_t)(now_ms - store->heard_ms[slot]) / NODE_HEALTH_TICK_MS;
    return (uint8_t)(ticks > 254u ? 254u : ticks);
}

#endif /* HYDRA_UMC_NODE_HEALTH_H */
