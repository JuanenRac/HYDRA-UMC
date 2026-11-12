/* =============================================================================
 * HYDRA-UMC Firmware - host test of nor_flash.h (the SPI NOR flash of the H757 board on SPI5)
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * The chip is a double that behaves like a NOR flash: a program only clears bits and wraps inside its page, an erase sets a sector to 0xFF, a program and an erase need the
 * write enable, they take effect when the chip select rises, and the status shows write-in-progress for a number of polls. It can also be absent, stuck busy, refuse the
 * write enable, lose bits when it programs, or make the transfer fail. Build with -DHYDRA_PLATFORM_H757. Nothing here touches the SPI peripheral.
 */
#include <stdio.h>
#include <string.h>
#include "nor_flash.h"

#if !defined(HYDRA_PLATFORM_H757)
#error "this test is for the H757 platform: build it with -DHYDRA_PLATFORM_H757"
#endif

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } } while (0)

static uint8_t memory[HYDRA_NOR_CAPACITY_BYTES];

typedef struct {
    int selected;
    uint32_t position;        /* bytes seen since the select */
    uint8_t opcode;
    uint32_t address;
    uint8_t page[HYDRA_NOR_PAGE_BYTES];
    uint32_t page_bytes;
    int wel;
    unsigned wip_polls;       /* polls of the status that still show write-in-progress */
    /* behaviours */
    unsigned busy_polls;      /* how long a program or an erase keeps the chip busy */
    int absent;               /* nothing answers: all ones */
    int stuck_busy;
    int refuse_write_enable;
    int lose_bits;            /* a program drops the lowest bit of each byte */
    unsigned fail_transfer_at;/* the transfer that fails (1-based, 0: never) */
    /* counts */
    unsigned transfers;
    unsigned max_page_chunk;  /* the longest program the chip received in one command */
} Chip;

static void op_select(void *ctx, int asserted)
{
    Chip *c = (Chip *)ctx;
    if (asserted) {
        c->selected = 1;
        c->position = 0;
        c->page_bytes = 0;
        c->opcode = 0;
        return;
    }
    /* the chip select rises: a command takes effect */
    if (c->selected && !c->absent) {
        if (c->opcode == NOR_CMD_WRITE_ENABLE && !c->refuse_write_enable) c->wel = 1;
        if (c->opcode == NOR_CMD_PAGE_PROGRAM && c->wel && c->page_bytes > 0u) {
            if (c->page_bytes > c->max_page_chunk) c->max_page_chunk = c->page_bytes;
            const uint32_t base = c->address - (c->address % HYDRA_NOR_PAGE_BYTES);
            for (uint32_t i = 0; i < c->page_bytes; i++) {
                const uint32_t at = base + ((c->address % HYDRA_NOR_PAGE_BYTES) + i) % HYDRA_NOR_PAGE_BYTES; /* wraps inside the page */
                uint8_t value = c->page[i];
                if (c->lose_bits) value &= 0xFEu;
                memory[at] &= value;                                                                           /* a program only clears bits */
            }
            c->wel = 0;
            c->wip_polls = c->busy_polls;
        }
        if (c->opcode == NOR_CMD_SECTOR_ERASE && c->wel && c->position == 4u) {
            memset(memory + (c->address - (c->address % HYDRA_NOR_SECTOR_BYTES)), 0xFF, HYDRA_NOR_SECTOR_BYTES);
            c->wel = 0;
            c->wip_polls = c->busy_polls;
        }
    }
    c->selected = 0;
}

static uint8_t chip_byte(Chip *c, uint8_t in)
{
    uint8_t out = 0xFFu;
    if (c->absent) return out;
    if (c->position == 0u) {
        c->opcode = in;
    } else if (c->opcode == NOR_CMD_JEDEC_ID) {
        static const uint8_t id[3] = {(uint8_t)(HYDRA_NOR_JEDEC_ID >> 16), (uint8_t)(HYDRA_NOR_JEDEC_ID >> 8), (uint8_t)HYDRA_NOR_JEDEC_ID};
        out = c->position <= 3u ? id[c->position - 1u] : 0xFFu;
    } else if (c->opcode == NOR_CMD_READ_STATUS) {
        const int busy = c->stuck_busy || c->wip_polls > 0u;
        out = (uint8_t)((busy ? NOR_STATUS_WIP : 0u) | (c->wel ? NOR_STATUS_WEL : 0u));
        if (!c->stuck_busy && c->wip_polls > 0u && c->position == 1u) c->wip_polls--;
    } else if (c->opcode == NOR_CMD_READ || c->opcode == NOR_CMD_PAGE_PROGRAM || c->opcode == NOR_CMD_SECTOR_ERASE) {
        if (c->position <= 3u) {
            c->address = (c->position == 1u) ? ((uint32_t)in << 16) : (c->position == 2u) ? (c->address | ((uint32_t)in << 8)) : (c->address | in);
        } else if (c->opcode == NOR_CMD_READ) {
            out = memory[(c->address + (c->position - 4u)) % HYDRA_NOR_CAPACITY_BYTES];
        } else if (c->opcode == NOR_CMD_PAGE_PROGRAM && c->page_bytes < HYDRA_NOR_PAGE_BYTES) {
            c->page[c->page_bytes++] = in;
        }
    }
    c->position++;
    return out;
}

static int op_transfer(void *ctx, const uint8_t *tx, uint8_t *rx, uint32_t length)
{
    Chip *c = (Chip *)ctx;
    c->transfers++;
    if (c->fail_transfer_at != 0u && c->transfers == c->fail_transfer_at) return 0;
    for (uint32_t i = 0; i < length; i++) {
        const uint8_t out = chip_byte(c, tx != 0 ? tx[i] : 0xFFu);
        if (rx != 0) rx[i] = out;
    }
    return 1;
}

static const NorOps OPS = {op_select, op_transfer};

static void chip_init(Chip *c)
{
    memset(c, 0, sizeof *c);
    c->busy_polls = 3u;
    memset(memory, 0xFF, sizeof memory);
}

int main(void)
{
    Chip chip;
    uint8_t scratch[HYDRA_NOR_PAGE_BYTES];
    uint8_t buffer[1024];

    /* the geometry of the profile */
    CHECK(HYDRA_NOR_CAPACITY_BYTES == 16UL * 1024UL * 1024UL && HYDRA_NOR_PAGE_BYTES == 256U && HYDRA_NOR_SECTOR_BYTES == 4096U, "16 MiB, pages of 256 bytes, sectors of 4 KiB");

    /* the identifier: the chip of the board, a different one, and no chip at all */
    chip_init(&chip);
    CHECK(Nor_Probe(&OPS, &chip) == NOR_OK, "probe: the chip of the board");
    uint32_t id = 0;
    CHECK(Nor_ReadId(&OPS, &chip, &id) == NOR_OK && id == HYDRA_NOR_JEDEC_ID, "the identifier is the one of the profile");
    chip_init(&chip);
    chip.absent = 1;
    CHECK(Nor_Probe(&OPS, &chip) == NOR_ERR_ID, "probe: an absent chip answers all ones and is refused");

    /* the ranges are checked before a byte goes out */
    chip_init(&chip);
    const unsigned before = chip.transfers;
    CHECK(Nor_Read(&OPS, &chip, HYDRA_NOR_CAPACITY_BYTES, buffer, 1) == NOR_ERR_RANGE, "read one past the end");
    CHECK(Nor_Read(&OPS, &chip, HYDRA_NOR_CAPACITY_BYTES - 4u, buffer, 5) == NOR_ERR_RANGE, "read across the end");
    CHECK(Nor_Write(&OPS, &chip, HYDRA_NOR_CAPACITY_BYTES - 4u, buffer, 5, 0, scratch) == NOR_ERR_RANGE, "write across the end");
    CHECK(Nor_EraseSector(&OPS, &chip, HYDRA_NOR_CAPACITY_BYTES) == NOR_ERR_RANGE, "erase one past the end");
    CHECK(Nor_Read(&OPS, &chip, 0xFFFFFFF0u, buffer, 0x20) == NOR_ERR_RANGE, "an address near the top of 32 bits does not wrap");
    CHECK(chip.transfers == before, "a refused range puts nothing on the bus");
    CHECK(Nor_Read(&OPS, &chip, HYDRA_NOR_CAPACITY_BYTES - 4u, buffer, 4) == NOR_OK, "read the last four bytes");
    CHECK(Nor_Read(&OPS, &chip, 0, buffer, 0) == NOR_OK, "a read of nothing is fine");
    CHECK(Nor_EraseSector(&OPS, &chip, 100) == NOR_ERR_ALIGN, "erase of an address that is not a sector start");

    /* a write cut at the page boundaries, read back, with the verify on */
    chip_init(&chip);
    for (unsigned i = 0; i < sizeof buffer; i++) buffer[i] = (uint8_t)(i * 5u + 1u);
    CHECK(Nor_Write(&OPS, &chip, 250, buffer, 600, 1, scratch) == NOR_OK, "write 600 bytes from 250 across pages, verified");
    uint8_t back[1024];
    CHECK(Nor_Read(&OPS, &chip, 250, back, 600) == NOR_OK && memcmp(back, buffer, 600) == 0, "read back the 600 bytes");
    CHECK(chip.max_page_chunk <= HYDRA_NOR_PAGE_BYTES, "no program command carried more than a page");
    CHECK(memory[249] == 0xFF && memory[850] == 0xFF, "the bytes around the range are untouched");

    /* a write that starts in the last bytes of a page does not wrap onto the start of the page */
    chip_init(&chip);
    CHECK(Nor_Write(&OPS, &chip, 254, buffer, 4, 1, scratch) == NOR_OK, "write four bytes across a page boundary");
    CHECK(memory[254] == buffer[0] && memory[255] == buffer[1] && memory[256] == buffer[2] && memory[257] == buffer[3] && memory[0] == 0xFF, "they land where they were asked, not at the start of the page");

    /* a program only clears bits: rewriting without an erase gives the AND of the two, and the verify says so */
    chip_init(&chip);
    const uint8_t first = 0xF0u, second = 0x0Fu;
    CHECK(Nor_Write(&OPS, &chip, 4096, &first, 1, 1, scratch) == NOR_OK, "write 0xF0");
    CHECK(Nor_Write(&OPS, &chip, 4096, &second, 1, 1, scratch) == NOR_ERR_VERIFY && memory[4096] == 0x00u, "rewrite without erase: the verify catches the AND");
    CHECK(Nor_EraseSector(&OPS, &chip, 4096) == NOR_OK && memory[4096] == 0xFFu && memory[8191] == 0xFFu, "erase sets the sector to ones");
    CHECK(Nor_Write(&OPS, &chip, 4096, &second, 1, 1, scratch) == NOR_OK && memory[4096] == 0x0Fu, "write after the erase");
    memory[8192] = 0x11u;
    CHECK(Nor_EraseSector(&OPS, &chip, 4096) == NOR_OK && memory[8192] == 0x11u, "an erase touches only its own sector");

    /* a chip that loses bits is caught by the verify */
    chip_init(&chip);
    chip.lose_bits = 1;
    CHECK(Nor_Write(&OPS, &chip, 0, buffer, 16, 1, scratch) == NOR_ERR_VERIFY, "lost bits: verify error");
    chip_init(&chip);
    chip.lose_bits = 1;
    CHECK(Nor_Write(&OPS, &chip, 0, buffer, 16, 0, scratch) == NOR_OK, "lost bits without verify: nobody notices (the reason the verify exists)");

    /* a chip that never leaves busy ends in a time-out, and one that refuses the write enable is reported */
    chip_init(&chip);
    chip.stuck_busy = 1;
    CHECK(Nor_Write(&OPS, &chip, 0, buffer, 4, 0, scratch) == NOR_ERR_TIMEOUT, "stuck busy: time-out");
    chip_init(&chip);
    chip.refuse_write_enable = 1;
    CHECK(Nor_Write(&OPS, &chip, 0, buffer, 4, 0, scratch) == NOR_ERR_NOT_ENABLED && memory[0] == 0xFFu, "write enable refused: nothing is programmed");
    CHECK(Nor_EraseSector(&OPS, &chip, 0) == NOR_ERR_NOT_ENABLED, "write enable refused: no erase");

    /* a failing peripheral ends the operation, whichever transfer fails */
    for (unsigned at = 1; at <= 6u; at++) {
        chip_init(&chip);
        chip.fail_transfer_at = at;
        CHECK(Nor_Write(&OPS, &chip, 0, buffer, 4, 1, scratch) == NOR_ERR_TRANSFER, "a failing transfer is reported");
    }

    /* the chip select is released at the end of every command, also when the transfer fails */
    chip_init(&chip);
    chip.fail_transfer_at = 1;
    (void)Nor_ReadId(&OPS, &chip, &id);
    CHECK(chip.selected == 0, "the chip select is high after a failed command");

    if (failures == 0) printf("NOR_FLASH=PASS\n");
    return failures == 0 ? 0 : 1;
}
