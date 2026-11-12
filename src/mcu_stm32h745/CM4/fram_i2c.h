/* =============================================================================
 * HYDRA-UMC Firmware - src/mcu_stm32h745/CM4/fram_i2c.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * fram_i2c.h - driver of the FM24CL64B F-RAM (8 KiB, I2C) on the local bus I2C4 of the main board (HAL-free, host-testable)
 *
 * The chip is U7 of the main board: SCL on PD12 and SDA on PD13 (I2C4), the three address pins to ground, so the 7-bit address is 0x50; the pull-ups are
 * 4.7 kOhm to 3V3. An access is the address byte (write), two address bytes (high first, 13 bits used), and then the data; a read repeats the start and
 * reads. An F-RAM has no page and no write delay: a write may be of any length and the address wraps at the end of the chip, so the chunking below is
 * only the size of the buffer of the bus layer, not a limit of the chip.
 *
 * The driver talks to a FramI2cBus_t of one function (`transfer`): it writes `tx` and, when `rx_length` is not 0, repeats the start and reads. The HAL
 * glue (SpiApp.c) and the fake bus of tests/test_fram_i2c.c both implement it. Every chunk is tried FRAM_I2C_RETRIES times, a write is read back and
 * compared chunk by chunk, and an optional `yield` runs between chunks so that the task that owns the bus keeps sending the frames of the emergency stop.
 * Nothing here has run on a board; docs/FRAM_ERASE.md and bench rows 30 and 31 of docs/BENCH_TESTS.md are the check that remains.
 * ============================================================================= */
#ifndef FRAM_I2C_H
#define FRAM_I2C_H

#include <stdint.h>
#include <stddef.h>
#include "fram_node.h"

#define FRAM_I2C_ADDRESS   0x50u  /* 7 bits: A2, A1 and A0 to ground */
#define FRAM_I2C_SIZE      8192u  /* FM24CL64B: 64 kbit */
#define FRAM_I2C_CHUNK     30u    /* data bytes per transfer: with the two address bytes the bus layer needs a buffer of 32 */
#define FRAM_I2C_RETRIES   3u     /* tries of one chunk before the access fails */
#define FRAM_I2C_TIMEOUT_MS 20u   /* handed to the bus layer for one transfer */

typedef struct {
    /* Writes tx_length bytes to the device and, when rx_length is not 0, repeats the start and reads rx_length bytes. Returns 0 on success, anything else
     * for a NACK, a timeout or an arbitration loss (the driver then tries again). */
    int (*transfer)(void *context, uint8_t address7, const uint8_t *tx, uint32_t tx_length, uint8_t *rx, uint32_t rx_length, uint32_t timeout_ms);
    /* Run between two chunks (may be NULL). */
    void (*yield)(void *context);
    void *context;
} FramI2cBus_t;

typedef struct {
    const FramI2cBus_t *bus;
    uint32_t retries_used; /* how many repeated tries the accesses needed, for the diagnostics */
} FramI2c_t;

static inline void FramI2c_Init(FramI2c_t *f, const FramI2cBus_t *bus)
{
    f->bus = bus;
    f->retries_used = 0;
}

static inline int FramI2c_ReadChunk(FramI2c_t *f, uint32_t address, uint8_t *data, uint32_t length)
{
    uint8_t tx[2];
    tx[0] = (uint8_t)(address >> 8);
    tx[1] = (uint8_t)address;
    for (uint32_t attempt = 0; attempt < FRAM_I2C_RETRIES; attempt++) {
        if (f->bus->transfer(f->bus->context, FRAM_I2C_ADDRESS, tx, 2u, data, length, FRAM_I2C_TIMEOUT_MS) == 0) return 0;
        f->retries_used++;
    }
    return 1;
}

static inline int FramI2c_Read(FramI2c_t *f, uint32_t address, uint8_t *data, uint32_t length)
{
    if (f == NULL || f->bus == NULL || f->bus->transfer == NULL || data == NULL) return 1;
    if (address > FRAM_I2C_SIZE || length > FRAM_I2C_SIZE - address) return 1; /* never wrap around the end of the chip by accident */
    while (length > 0) {
        const uint32_t n = length > FRAM_I2C_CHUNK ? FRAM_I2C_CHUNK : length;
        if (FramI2c_ReadChunk(f, address, data, n) != 0) return 1;
        address += n;
        data += n;
        length -= n;
        if (length > 0 && f->bus->yield != NULL) f->bus->yield(f->bus->context);
    }
    return 0;
}

/* Writes and reads back, chunk by chunk: a chunk that does not read back as written is tried again as a whole and then fails the write. */
static inline int FramI2c_Write(FramI2c_t *f, uint32_t address, const uint8_t *data, uint32_t length)
{
    uint8_t tx[2u + FRAM_I2C_CHUNK];
    uint8_t back[FRAM_I2C_CHUNK];
    if (f == NULL || f->bus == NULL || f->bus->transfer == NULL || data == NULL) return 1;
    if (address > FRAM_I2C_SIZE || length > FRAM_I2C_SIZE - address) return 1;
    while (length > 0) {
        const uint32_t n = length > FRAM_I2C_CHUNK ? FRAM_I2C_CHUNK : length;
        int done = 0;
        tx[0] = (uint8_t)(address >> 8);
        tx[1] = (uint8_t)address;
        for (uint32_t i = 0; i < n; i++) tx[2u + i] = data[i];
        for (uint32_t attempt = 0; attempt < FRAM_I2C_RETRIES && !done; attempt++) {
            if (f->bus->transfer(f->bus->context, FRAM_I2C_ADDRESS, tx, 2u + n, NULL, 0u, FRAM_I2C_TIMEOUT_MS) == 0 &&
                FramI2c_ReadChunk(f, address, back, n) == 0) {
                done = 1;
                for (uint32_t i = 0; i < n; i++) {
                    if (back[i] != data[i]) { done = 0; break; }
                }
            }
            if (!done) f->retries_used++;
        }
        if (!done) return 1;
        address += n;
        data += n;
        length -= n;
        if (length > 0 && f->bus->yield != NULL) f->bus->yield(f->bus->context);
    }
    return 0;
}

static inline int FramI2c_DriverRead(void *context, uint32_t address, uint8_t *data, uint32_t length)
{
    return FramI2c_Read((FramI2c_t *)context, address, data, length);
}

static inline int FramI2c_DriverWrite(void *context, uint32_t address, const uint8_t *data, uint32_t length)
{
    return FramI2c_Write((FramI2c_t *)context, address, data, length);
}

/* The driver that fram_node.h talks to. */
static inline void FramI2c_MakeDriver(FramDriver_t *driver, FramI2c_t *f)
{
    driver->size = FRAM_I2C_SIZE;
    driver->read = FramI2c_DriverRead;
    driver->write = FramI2c_DriverWrite;
    driver->context = f;
}

#endif /* FRAM_I2C_H */
