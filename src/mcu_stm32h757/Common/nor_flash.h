// =============================================================================
// HYDRA-UMC Kinematic Brain - driver of the SPI NOR flash of the H757 board (GD25Q128E on SPI5), header-only and free of the HAL
// Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
// GPL-3.0-or-later - see repo root LICENSE
//
// A standard-SPI NOR flash, not memory mapped: every access is a command on SPI5 (6.25 Mbit/s in the board project). The driver uses the command set common to the family of
// 25-series NOR chips: write enable, read status, read data, page program, sector erase and the JEDEC identifier. It reaches the chip only through the callbacks of NorOps
// (chip select and a full-duplex transfer), so the host test runs it against a double that behaves like a NOR flash: a program only clears bits, an erase sets a sector to
// 0xFF, a program or an erase needs the write enable, and the status shows the write-in-progress bit for a number of polls.
//
// Rules the driver keeps: an address range outside the chip is refused before a byte goes out; a write is cut at every page boundary because a page program wraps inside the
// page; a write or an erase waits for the write-in-progress bit to fall and gives up after a limit of polls (the chip may be absent or stuck); `Nor_Write` can read back what
// it wrote. The geometry and the identifier come from the platform profile. The command set is that of the family and is still to be confirmed with the datasheet of the
// chip. Only for the H757 platform. Nothing here has run on a board.
// =============================================================================
#ifndef HYDRA_NOR_FLASH_H
#define HYDRA_NOR_FLASH_H

#include <stdint.h>
#include "platform_profile.h"

#if defined(HYDRA_PLATFORM_H757)

#define NOR_CMD_WRITE_ENABLE 0x06u
#define NOR_CMD_READ_STATUS  0x05u
#define NOR_CMD_READ         0x03u
#define NOR_CMD_PAGE_PROGRAM 0x02u
#define NOR_CMD_SECTOR_ERASE 0x20u
#define NOR_CMD_JEDEC_ID     0x9Fu
#define NOR_STATUS_WIP       0x01u
#define NOR_STATUS_WEL       0x02u

_Static_assert(HYDRA_NOR_PAGE_BYTES == 256U, "the page program of this driver is 256 bytes");
_Static_assert(HYDRA_NOR_SECTOR_BYTES % HYDRA_NOR_PAGE_BYTES == 0U, "a sector holds whole pages");
_Static_assert(HYDRA_NOR_CAPACITY_BYTES % HYDRA_NOR_SECTOR_BYTES == 0UL, "the chip holds whole sectors");
_Static_assert(HYDRA_NOR_CAPACITY_BYTES <= 0x1000000UL, "three address bytes reach 16 MiB");

typedef enum {
    NOR_OK = 0,
    NOR_ERR_RANGE = 1,       /* the range is outside the chip */
    NOR_ERR_ALIGN = 2,       /* an erase address that is not the start of a sector */
    NOR_ERR_TRANSFER = 3,    /* the SPI peripheral failed */
    NOR_ERR_TIMEOUT = 4,     /* write in progress never fell */
    NOR_ERR_NOT_ENABLED = 5, /* the write enable latch did not set */
    NOR_ERR_ID = 6,          /* the identifier is not the one of the chip on the board */
    NOR_ERR_VERIFY = 7       /* the read-back differs from what was written */
} NorStatus;

typedef struct {
    void (*select)(void *ctx, int asserted);                                                   /* the chip select: 1 low, 0 high */
    int (*transfer)(void *ctx, const uint8_t *tx, uint8_t *rx, uint32_t length);              /* full duplex, 1 on success; rx may be 0 */
} NorOps;

#define NOR_MAX_COMMAND 4u

static inline NorStatus Nor_Command(const NorOps *ops, void *ctx, const uint8_t *command, uint32_t command_bytes, const uint8_t *write_data, uint8_t *read_data, uint32_t data_bytes)
{
    NorStatus status = NOR_OK;
    ops->select(ctx, 1);
    if (!ops->transfer(ctx, command, 0, command_bytes)) status = NOR_ERR_TRANSFER;
    if (status == NOR_OK && data_bytes != 0u) {
        if (write_data != 0) {
            if (!ops->transfer(ctx, write_data, 0, data_bytes)) status = NOR_ERR_TRANSFER;
        } else if (!ops->transfer(ctx, 0, read_data, data_bytes)) {
            status = NOR_ERR_TRANSFER;
        }
    }
    ops->select(ctx, 0);
    return status;
}

static inline NorStatus Nor_ReadStatus(const NorOps *ops, void *ctx, uint8_t *status_register)
{
    const uint8_t command[1] = {NOR_CMD_READ_STATUS};
    return Nor_Command(ops, ctx, command, 1u, 0, status_register, 1u);
}

static inline NorStatus Nor_WaitIdle(const NorOps *ops, void *ctx)
{
    for (unsigned long poll = 0; poll < HYDRA_NOR_BUSY_POLL_LIMIT; poll++) {
        uint8_t sr = 0;
        const NorStatus status = Nor_ReadStatus(ops, ctx, &sr);
        if (status != NOR_OK) return status;
        if ((sr & NOR_STATUS_WIP) == 0u) return NOR_OK;
    }
    return NOR_ERR_TIMEOUT;
}

static inline NorStatus Nor_WriteEnable(const NorOps *ops, void *ctx)
{
    const uint8_t command[1] = {NOR_CMD_WRITE_ENABLE};
    NorStatus status = Nor_Command(ops, ctx, command, 1u, 0, 0, 0u);
    if (status != NOR_OK) return status;
    uint8_t sr = 0;
    status = Nor_ReadStatus(ops, ctx, &sr);
    if (status != NOR_OK) return status;
    return (sr & NOR_STATUS_WEL) ? NOR_OK : NOR_ERR_NOT_ENABLED;
}

static inline NorStatus Nor_ReadId(const NorOps *ops, void *ctx, uint32_t *id)
{
    const uint8_t command[1] = {NOR_CMD_JEDEC_ID};
    uint8_t bytes[3] = {0, 0, 0};
    const NorStatus status = Nor_Command(ops, ctx, command, 1u, 0, bytes, 3u);
    if (status != NOR_OK) return status;
    *id = ((uint32_t)bytes[0] << 16) | ((uint32_t)bytes[1] << 8) | bytes[2];
    return NOR_OK;
}

/* The identifier must be the one of the chip on the board: a wrong or absent chip (all ones or all zeros) is refused before anything is written. */
static inline NorStatus Nor_Probe(const NorOps *ops, void *ctx)
{
    uint32_t id = 0;
    const NorStatus status = Nor_ReadId(ops, ctx, &id);
    if (status != NOR_OK) return status;
    return id == HYDRA_NOR_JEDEC_ID ? NOR_OK : NOR_ERR_ID;
}

static inline int Nor_InRange(uint32_t address, uint32_t length)
{
    return length <= HYDRA_NOR_CAPACITY_BYTES && address <= HYDRA_NOR_CAPACITY_BYTES - length;
}

static inline void Nor_PutAddress(uint8_t *command, uint8_t opcode, uint32_t address)
{
    command[0] = opcode;
    command[1] = (uint8_t)(address >> 16);
    command[2] = (uint8_t)(address >> 8);
    command[3] = (uint8_t)address;
}

static inline NorStatus Nor_Read(const NorOps *ops, void *ctx, uint32_t address, uint8_t *data, uint32_t length)
{
    if (!Nor_InRange(address, length)) return NOR_ERR_RANGE;
    if (length == 0u) return NOR_OK;
    uint8_t command[NOR_MAX_COMMAND];
    Nor_PutAddress(command, NOR_CMD_READ, address);
    return Nor_Command(ops, ctx, command, NOR_MAX_COMMAND, 0, data, length);
}

static inline NorStatus Nor_EraseSector(const NorOps *ops, void *ctx, uint32_t address)
{
    if (!Nor_InRange(address, HYDRA_NOR_SECTOR_BYTES)) return NOR_ERR_RANGE;
    if (address % HYDRA_NOR_SECTOR_BYTES != 0u) return NOR_ERR_ALIGN;
    NorStatus status = Nor_WriteEnable(ops, ctx);
    if (status != NOR_OK) return status;
    uint8_t command[NOR_MAX_COMMAND];
    Nor_PutAddress(command, NOR_CMD_SECTOR_ERASE, address);
    status = Nor_Command(ops, ctx, command, NOR_MAX_COMMAND, 0, 0, 0u);
    if (status != NOR_OK) return status;
    return Nor_WaitIdle(ops, ctx);
}

/* Writes into erased bytes, cut at every page boundary. With `verify` the range is read back and compared; scratch must hold HYDRA_NOR_PAGE_BYTES bytes. */
static inline NorStatus Nor_Write(const NorOps *ops, void *ctx, uint32_t address, const uint8_t *data, uint32_t length, int verify, uint8_t *scratch)
{
    if (!Nor_InRange(address, length)) return NOR_ERR_RANGE;
    uint32_t done = 0;
    while (done < length) {
        const uint32_t here = address + done;
        uint32_t chunk = HYDRA_NOR_PAGE_BYTES - (here % HYDRA_NOR_PAGE_BYTES);
        if (chunk > length - done) chunk = length - done;
        NorStatus status = Nor_WriteEnable(ops, ctx);
        if (status != NOR_OK) return status;
        uint8_t command[NOR_MAX_COMMAND];
        Nor_PutAddress(command, NOR_CMD_PAGE_PROGRAM, here);
        status = Nor_Command(ops, ctx, command, NOR_MAX_COMMAND, data + done, 0, chunk);
        if (status != NOR_OK) return status;
        status = Nor_WaitIdle(ops, ctx);
        if (status != NOR_OK) return status;
        if (verify) {
            status = Nor_Read(ops, ctx, here, scratch, chunk);
            if (status != NOR_OK) return status;
            for (uint32_t i = 0; i < chunk; i++) {
                if (scratch[i] != data[done + i]) return NOR_ERR_VERIFY;
            }
        }
        done += chunk;
    }
    return NOR_OK;
}

#endif // HYDRA_PLATFORM_H757

#endif // HYDRA_NOR_FLASH_H
