/* =============================================================================
 * HYDRA-UMC Firmware - src/mcu_stm32h757/CM4/fram_node.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * fram_node.h - how the kinematic brain answers the F-RAM erase and self-test requests (HAL-free, host-testable)
 *
 * The chip is the FM24CL64B (8 KiB, I2C) on the main board, on the local bus I2C4 of the H745 (fram_i2c.h). The requests reach the CM4 over the SPI link as the
 * frame types SPI_APP_FRAM_REQUEST and SPI_APP_FRAM_RESPONSE of the tier SELF (spi_app.h) and never go on a CAN bus. The logic knows nothing about the chip:
 * it talks to a FramDriver_t of two functions, which fram_i2c.h builds. With the switch HYDRA_FRAM_ERASE off (the default) it answers DISABLED; with no
 * driver (no I2C timing fixed) it answers NO_CHIP. Nothing here has run on a board. docs/FRAM_ERASE.md is the contract; tests/test_fram_node.c and
 * tests/test_fram_i2c.c replay it against this file with a simulated chip.
 *
 * Request  (host to board, DLC 8): 0xFA, operation, board (always FRAM_BRAIN_ID: the brain has the one chip), 'E' 'R' 'A' 'S' (erase) or 'T' 'E' 'S' 'T' (self-test), complement of the XOR of the first seven bytes.
 * Response (board to host, DLC 8): 0xFB, operation, status, detail, four bytes big endian (bytes written for an erase, address that failed for a test).
 * ============================================================================= */
#ifndef FRAM_NODE_H
#define FRAM_NODE_H

#include <stdint.h>
#include <stddef.h>

#ifndef HYDRA_FRAM_ERASE
#define HYDRA_FRAM_ERASE 0 /* the switch: off by default, the board answers DISABLED */
#endif

#define FRAM_REQUEST_MAGIC  0xFAu
#define FRAM_RESPONSE_MAGIC 0xFBu

#define FRAM_BRAIN_ID 0u /* the one F-RAM of the stack is the brain's: the board byte of a request must be this */

#define FRAM_OP_ERASE    0x01u
#define FRAM_OP_SELFTEST 0x02u

#define FRAM_STATUS_OK          0x00u
#define FRAM_STATUS_DISABLED    0x01u /* the switch is off */
#define FRAM_STATUS_NO_CHIP     0x02u /* no driver: the chip is not fixed or not fitted */
#define FRAM_STATUS_BAD_REQUEST 0x03u
#define FRAM_STATUS_WRONG_SLOT  0x04u /* the board byte is not FRAM_BRAIN_ID */
#define FRAM_STATUS_BUSY        0x05u /* an update is running: the brain does not touch the F-RAM meanwhile */
#define FRAM_STATUS_CHIP_ERROR  0x06u /* the driver reported a failure */
#define FRAM_STATUS_VERIFY_FAIL 0x07u /* the readback is not what was written */

#define FRAM_CHUNK_BYTES 32u
#define FRAM_SELFTEST_BYTES 64u /* the scratch area at the end of the chip: saved, written with two patterns, read back and restored */

typedef struct {
    uint32_t size;
    int (*read)(void *context, uint32_t address, uint8_t *data, uint32_t length);  /* 0 on success */
    int (*write)(void *context, uint32_t address, const uint8_t *data, uint32_t length); /* 0 on success */
    void *context;
} FramDriver_t;

typedef struct {
    const FramDriver_t *driver; /* NULL until the chip is fixed */
    uint8_t enabled;            /* HYDRA_FRAM_ERASE */
} FramNode_t;

static inline void FramNode_Init(FramNode_t *n, const FramDriver_t *driver)
{
    n->driver = driver;
    n->enabled = (uint8_t)(HYDRA_FRAM_ERASE ? 1u : 0u);
}

static inline uint8_t Fram_Check(const uint8_t *data)
{
    uint8_t x = 0;
    for (uint8_t i = 0; i < 7u; i++) x = (uint8_t)(x ^ data[i]);
    return (uint8_t)(x ^ 0xFFu);
}

static inline void Fram_BuildRequest(uint8_t out[8], uint8_t operation, uint8_t slot)
{
    out[0] = FRAM_REQUEST_MAGIC;
    out[1] = operation;
    out[2] = slot;
    if (operation == FRAM_OP_ERASE) {
        out[3] = 'E'; out[4] = 'R'; out[5] = 'A'; out[6] = 'S';
    } else {
        out[3] = 'T'; out[4] = 'E'; out[5] = 'S'; out[6] = 'T';
    }
    out[7] = Fram_Check(out);
}

static inline void Fram_BuildResponse(uint8_t out[8], uint8_t operation, uint8_t status, uint8_t detail, uint32_t value)
{
    out[0] = FRAM_RESPONSE_MAGIC;
    out[1] = operation;
    out[2] = status;
    out[3] = detail;
    out[4] = (uint8_t)(value >> 24);
    out[5] = (uint8_t)(value >> 16);
    out[6] = (uint8_t)(value >> 8);
    out[7] = (uint8_t)value;
}

/* 1 when the frame is a well-formed request: magic, a known operation, the word that goes with it and the check byte. */
static inline uint8_t Fram_DecodeRequest(const uint8_t *data, uint8_t dlc, uint8_t *operation, uint8_t *slot)
{
    if (dlc != 8u || data[0] != FRAM_REQUEST_MAGIC) return 0;
    if (data[7] != Fram_Check(data)) return 0;
    if (data[1] == FRAM_OP_ERASE) {
        if (data[3] != 'E' || data[4] != 'R' || data[5] != 'A' || data[6] != 'S') return 0;
    } else if (data[1] == FRAM_OP_SELFTEST) {
        if (data[3] != 'T' || data[4] != 'E' || data[5] != 'S' || data[6] != 'T') return 0;
    } else {
        return 0;
    }
    *operation = data[1];
    *slot = data[2];
    return 1;
}

/* An erase writes zeros over the whole chip (an F-RAM has nothing to erase), a chunk at a time, then reads it all back. */
static inline uint8_t FramNode_Erase(const FramDriver_t *d, uint32_t *written)
{
    uint8_t zeros[FRAM_CHUNK_BYTES];
    uint8_t back[FRAM_CHUNK_BYTES];
    for (uint32_t i = 0; i < FRAM_CHUNK_BYTES; i++) zeros[i] = 0;
    *written = 0;
    for (uint32_t address = 0; address < d->size; address += FRAM_CHUNK_BYTES) {
        uint32_t length = d->size - address;
        if (length > FRAM_CHUNK_BYTES) length = FRAM_CHUNK_BYTES;
        if (d->write(d->context, address, zeros, length) != 0) return FRAM_STATUS_CHIP_ERROR;
        *written = address + length;
    }
    for (uint32_t address = 0; address < d->size; address += FRAM_CHUNK_BYTES) {
        uint32_t length = d->size - address;
        if (length > FRAM_CHUNK_BYTES) length = FRAM_CHUNK_BYTES;
        if (d->read(d->context, address, back, length) != 0) return FRAM_STATUS_CHIP_ERROR;
        for (uint32_t i = 0; i < length; i++) {
            if (back[i] != 0u) {
                *written = address + i;
                return FRAM_STATUS_VERIFY_FAIL;
            }
        }
    }
    return FRAM_STATUS_OK;
}

/* The self-test keeps what was in the scratch area: it reads it, writes 0x55 and 0xAA over it and checks each readback, then puts the original back. */
static inline uint8_t FramNode_SelfTest(const FramDriver_t *d, uint32_t *failed_at)
{
    uint8_t saved[FRAM_SELFTEST_BYTES];
    uint8_t pattern[FRAM_SELFTEST_BYTES];
    uint8_t back[FRAM_SELFTEST_BYTES];
    uint32_t base;
    uint8_t status = FRAM_STATUS_OK;
    *failed_at = 0;
    if (d->size < FRAM_SELFTEST_BYTES) return FRAM_STATUS_CHIP_ERROR;
    base = d->size - FRAM_SELFTEST_BYTES;
    if (d->read(d->context, base, saved, FRAM_SELFTEST_BYTES) != 0) return FRAM_STATUS_CHIP_ERROR;
    for (uint32_t round = 0; round < 2u && status == FRAM_STATUS_OK; round++) {
        uint8_t value = round == 0u ? 0x55u : 0xAAu;
        for (uint32_t i = 0; i < FRAM_SELFTEST_BYTES; i++) pattern[i] = value;
        if (d->write(d->context, base, pattern, FRAM_SELFTEST_BYTES) != 0 || d->read(d->context, base, back, FRAM_SELFTEST_BYTES) != 0) {
            status = FRAM_STATUS_CHIP_ERROR;
            break;
        }
        for (uint32_t i = 0; i < FRAM_SELFTEST_BYTES; i++) {
            if (back[i] != value) {
                *failed_at = base + i;
                status = FRAM_STATUS_VERIFY_FAIL;
                break;
            }
        }
    }
    if (d->write(d->context, base, saved, FRAM_SELFTEST_BYTES) != 0 && status == FRAM_STATUS_OK) status = FRAM_STATUS_CHIP_ERROR;
    return status;
}

/* Feeds a frame received at the F-RAM offsets of this board's own slot window (0x20 erase request, 0x22 self-test request). Returns 1 and the response
 * (to be sent at offset request + 1) when a request was answered; 0 when the frame is not a request at all (nothing is sent for noise). */
static inline uint8_t FramNode_Handle(FramNode_t *n, uint8_t board_id, uint8_t update_running, const uint8_t *data, uint8_t dlc, uint8_t out[8])
{
    uint8_t operation;
    uint8_t slot;
    uint32_t value = 0;
    uint8_t status;
    if (!Fram_DecodeRequest(data, dlc, &operation, &slot)) return 0;
    if (!n->enabled) status = FRAM_STATUS_DISABLED;
    else if (slot != board_id) status = FRAM_STATUS_WRONG_SLOT;
    else if (update_running) status = FRAM_STATUS_BUSY;
    else if (n->driver == NULL || n->driver->size == 0u || n->driver->read == NULL || n->driver->write == NULL) status = FRAM_STATUS_NO_CHIP;
    else if (operation == FRAM_OP_ERASE) status = FramNode_Erase(n->driver, &value);
    else status = FramNode_SelfTest(n->driver, &value);
    Fram_BuildResponse(out, operation, status, board_id, value);
    return 1;
}

#endif /* FRAM_NODE_H */
