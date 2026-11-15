/* =============================================================================
 * HYDRA-UMC Firmware - tests/test_nor_backup_glue.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Host test of the glue that puts the copy of the 768 KB slot on the NOR flash (src/mcu_stm32h757/Common/nor_backup_glue.h): the NorbOps of the plan built from the NorOps of
 * the chip (a double that plays a 25-series chip at the level of the SPI commands) and from a RAM double of the flash layer. It runs the plan end to end through the glue: a save, a
 * restore, a staged update that is committed, the refusal of a wrong chip, of an erase that is not on a sector and of a chip that stops answering. Evidence: simulator.
 *
 *   cc -std=c11 -Wall -Wextra -Werror -DHYDRA_PLATFORM_H757 -Isrc/mcu_stm32h757/Common -o test_nor_backup_glue tests/test_nor_backup_glue.c
 */
#include <stdio.h>
#include <string.h>

#include "nor_backup_glue.h"

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } } while (0)

#define CHIP_BYTES (256UL * 1024UL)
#define SLOT (16UL * 1024UL)

static uint8_t chip[CHIP_BYTES];
static uint8_t status_reg;
static int wip_polls, fail_transfers, chip_id_ok = 1;
static uint8_t command[300];
static uint32_t command_len;
static uint8_t flash[SLOT];

static uint32_t address_of(void) { return ((uint32_t)command[1] << 16) | ((uint32_t)command[2] << 8) | command[3]; }

static void d_select(void *ctx, int asserted)
{
    (void)ctx;
    if (asserted) command_len = 0;
}

static int d_transfer(void *ctx, const uint8_t *tx, uint8_t *rx, uint32_t length)
{
    uint32_t i;
    (void)ctx;
    if (fail_transfers) return 0;
    if (tx != 0) {
        for (i = 0; i < length; i++) {
            command[command_len++] = tx[i];
            if (command_len == 1u && tx[i] == 0x06u) {
                status_reg |= 0x02u;
            } else if (command_len == 4u && command[0] == 0x20u && (status_reg & 0x02u)) {
                memset(chip + (address_of() & ~0xFFFu), 0xFF, 4096u);
                status_reg &= (uint8_t)~0x02u;
                wip_polls = 1;
            } else if (command_len > 4u && command[0] == 0x02u && (status_reg & 0x02u)) {
                const uint32_t base = address_of();
                chip[((base & ~0xFFu) | ((base + (command_len - 5u)) & 0xFFu)) % CHIP_BYTES] &= tx[i];
                if (i + 1u == length) {
                    status_reg &= (uint8_t)~0x02u;
                    wip_polls = 1;
                }
            }
        }
    } else {
        for (i = 0; i < length; i++) {
            if (command[0] == 0x9Fu) {
                static const uint8_t good[3] = {0xC8, 0x40, 0x18}, wrong[3] = {0xFF, 0xFF, 0xFF};
                rx[i] = (chip_id_ok ? good : wrong)[i % 3u];
            } else if (command[0] == 0x05u) {
                rx[i] = status_reg | (uint8_t)(wip_polls > 0 ? 1u : 0u);
                if (wip_polls > 0) wip_polls--;
            } else if (command[0] == 0x03u) {
                rx[i] = chip[(address_of() + i) % CHIP_BYTES];
            }
        }
    }
    return 1;
}

static const NorOps nor_ops = {d_select, d_transfer};

static int f_read(void *ctx, uint32_t o, uint8_t *d, uint32_t n) { (void)ctx; if (o + n > SLOT) return 1; memcpy(d, flash + o, n); return 0; }
static int f_erase(void *ctx, uint32_t n) { (void)ctx; if (n > SLOT) return 1; memset(flash, 0xFF, SLOT); return 0; }
static int f_write(void *ctx, uint32_t o, const uint8_t *d, uint32_t n) { (void)ctx; if (o + n > SLOT) return 1; memcpy(flash + o, d, n); return 0; }

static void reset(void)
{
    memset(chip, 0xFF, sizeof chip);
    memset(flash, 0xFF, sizeof flash);
    status_reg = 0;
    wip_polls = 0;
    fail_transfers = 0;
    chip_id_ok = 1;
}

int main(void)
{
    NorbGlue glue;
    NorbOps ops;
    NorbConfig c;
    const NorbFlashLayer layer = {f_read, f_erase, f_write, 0};
    uint8_t image[9000], back[256];
    uint32_t got = 0, i;

    Norb_Defaults(&c);
    c.nor_offset = 0;
    c.nor_size = HYDRA_NOR_CAPACITY_BYTES;
    c.slot_bytes = SLOT;
    for (i = 0; i < sizeof image; i++) image[i] = (uint8_t)(i * 11u + 1u);

    /* a wrong chip is refused before an operation exists */
    reset();
    chip_id_ok = 0;
    CHECK(NorbGlue_Open(&glue, &nor_ops, 0, &layer, &ops) == NOR_ERR_ID, "a chip with another identifier is refused");
    fail_transfers = 1;
    CHECK(NorbGlue_Open(&glue, &nor_ops, 0, &layer, &ops) == NOR_ERR_TRANSFER, "a chip that does not answer is refused");

    reset();
    CHECK(NorbGlue_Open(&glue, &nor_ops, 0, &layer, &ops) == NOR_OK, "the chip of the board is accepted");

    /* a save and a restore through the glue */
    memcpy(flash, image, sizeof image);
    CHECK(Norb_Save(&ops, &c, sizeof image) == NORB_OK, "the application is saved into the NOR");
    CHECK(Norb_CopyExists(&ops, &c), "the copy exists");
    memset(flash, 0, sizeof flash);
    CHECK(Norb_Restore(&ops, &c, &got) == NORB_OK && got == sizeof image && memcmp(flash, image, sizeof image) == 0, "the copy restores the application");

    /* a staged update: pages, the check, the header and the commit */
    {
        uint8_t update[10000];
        uint32_t offset, crc;
        for (i = 0; i < sizeof update; i++) update[i] = (uint8_t)(i * 3u + 9u);
        crc = Norb_Crc32(update, sizeof update);
        CHECK(Norb_StageBegin(&ops, &c, sizeof update) == NORB_OK, "staging begins");
        for (offset = 0; offset < sizeof update; offset += 256) {
            const uint32_t n = sizeof update - offset < 256 ? (uint32_t)sizeof update - offset : 256;
            CHECK(Norb_StagePage(&ops, &c, offset, update + offset, n) == NORB_OK, "a page is staged and read back");
        }
        CHECK(Norb_StageFinish(&ops, &c, sizeof update, crc) == NORB_OK, "the update is complete");
        memset(flash, 0, sizeof flash);
        CHECK(Norb_Restore(&ops, &c, &got) == NORB_OK && got == sizeof update && memcmp(flash, update, sizeof update) == 0, "the staged update is committed into the slot");
    }

    /* the operations of the glue refuse what they must */
    CHECK(ops.nor_erase(ops.ctx, 100u, 4096u) == 1, "an erase that does not start on a sector is refused");
    CHECK(ops.nor_erase(ops.ctx, 0u, 0u) == 0, "an erase of nothing does nothing");
    CHECK(ops.nor_read(ops.ctx, HYDRA_NOR_CAPACITY_BYTES, back, 1u) == 1, "a read outside the chip is refused");
    CHECK(ops.nor_write(ops.ctx, HYDRA_NOR_CAPACITY_BYTES, back, 1u) == 1, "a write outside the chip is refused");
    CHECK(ops.nor_erase(ops.ctx, HYDRA_NOR_CAPACITY_BYTES, 4096u) == 1, "an erase outside the chip is refused");
    CHECK(ops.flash_read(ops.ctx, SLOT, back, 1u) == 1, "a flash read outside the slot is refused by the layer");
    CHECK(ops.flash_erase(ops.ctx, SLOT + 1u) == 1, "a flash erase longer than the slot is refused by the layer");
    CHECK(ops.flash_write(ops.ctx, SLOT, back, 1u) == 1, "a flash write outside the slot is refused by the layer");

    /* a chip that stops answering */
    fail_transfers = 1;
    CHECK(Norb_Save(&ops, &c, 100u) != NORB_OK, "a save onto a chip that does not answer fails");
    CHECK(!Norb_CopyExists(&ops, &c), "and no copy is believed");
    fail_transfers = 0;

    if (failures) {
        printf("nor_backup_glue: %d FAILED\n", failures);
        return 1;
    }
    printf("nor_backup_glue host logic tests passed\n");
    return 0;
}
