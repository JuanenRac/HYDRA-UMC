/* =============================================================================
 * HYDRA-UMC Firmware - tests/test_nor_backup_plan.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Host test of the backup copy of the CM7 application in the NOR flash (src/mcu_stm32h757/Common/nor_backup_plan.h): the CRC-32 against the literal check value, the layout and
 * its refusals, the header (literal bytes, damaged magic, CRC, length), the choice of the newer header, save and restore against two RAM doubles (a NOR with erase-before-write and a
 * flash), and a cut of the power at every operation of a save: the application of the flash is never touched and a copy that is restored is always the image that was saved.
 * Evidence: simulator.
 *
 *   cc -std=c11 -Wall -Wextra -Werror -Isrc/mcu_stm32h757/Common -o test_nor_backup_plan tests/test_nor_backup_plan.c
 */
#include <stdio.h>
#include <string.h>

#include "nor_backup_plan.h"

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } } while (0)

#define NOR_SIZE (1024UL * 1024UL)
#define SLOT (16UL * 1024UL)
#define FLASH_SIZE SLOT

static uint8_t nor[NOR_SIZE];
static uint8_t flash[FLASH_SIZE];
static long ops_left = -1; /* -1: never fail; otherwise fail every operation once it reaches 0 */
static long ops_done;
static int fail_flash_write_at = -1, fail_nor_read_at = -1, fail_flash_read_at = -1, flash_corrupt = 0, flash_read_calls, nor_read_calls, flash_write_calls;

static int tick(void)
{
    ops_done++;
    if (ops_left < 0) return 0;
    if (ops_left == 0) return 1;
    ops_left--;
    return 0;
}

static int d_nor_read(void *ctx, uint32_t o, uint8_t *d, uint32_t n)
{
    (void)ctx;
    if (fail_nor_read_at >= 0 && nor_read_calls++ == fail_nor_read_at) return 1;
    if (o + n > NOR_SIZE) return 1;
    memcpy(d, nor + o, n);
    return 0;
}
static int d_nor_erase(void *ctx, uint32_t o, uint32_t n)
{
    (void)ctx;
    if (tick() || o + n > NOR_SIZE) return 1;
    memset(nor + o, 0xFF, n);
    return 0;
}
static int d_nor_write(void *ctx, uint32_t o, const uint8_t *d, uint32_t n)
{
    uint32_t i;
    (void)ctx;
    if (tick() || o + n > NOR_SIZE) return 1;
    for (i = 0; i < n; i++) nor[o + i] &= d[i]; /* a NOR can only clear bits: writing over a non-erased byte corrupts it */
    return 0;
}
static int d_flash_read(void *ctx, uint32_t o, uint8_t *d, uint32_t n)
{
    (void)ctx;
    if (fail_flash_read_at >= 0 && flash_read_calls++ == fail_flash_read_at) return 1;
    if (o + n > FLASH_SIZE) return 1;
    memcpy(d, flash + o, n);
    return 0;
}
static int d_flash_erase(void *ctx, uint32_t n)
{
    (void)ctx;
    if (n > FLASH_SIZE) return 1;
    memset(flash, 0xFF, FLASH_SIZE);
    return 0;
}
static int d_flash_write(void *ctx, uint32_t o, const uint8_t *d, uint32_t n)
{
    (void)ctx;
    if (fail_flash_write_at >= 0 && flash_write_calls++ == fail_flash_write_at) return 1;
    if (o + n > FLASH_SIZE) return 1;
    memcpy(flash + o, d, n);
    if (flash_corrupt) flash[o] ^= 1u;
    return 0;
}

static NorbOps ops = {d_nor_read, d_nor_erase, d_nor_write, d_flash_read, d_flash_erase, d_flash_write, 0};

static void reset(void)
{
    memset(nor, 0xFF, sizeof nor);
    memset(flash, 0xFF, sizeof flash);
    ops_left = -1;
    ops_done = 0;
    fail_flash_write_at = fail_nor_read_at = fail_flash_read_at = -1;
    flash_corrupt = 0;
    flash_read_calls = nor_read_calls = flash_write_calls = 0;
}

static NorbConfig cfg(void)
{
    NorbConfig c;
    Norb_Defaults(&c);
    c.nor_offset = 65536UL;
    c.nor_size = NOR_SIZE;
    c.slot_bytes = SLOT;
    return c;
}

static void fill(uint8_t seed, uint32_t length)
{
    uint32_t i;
    for (i = 0; i < length; i++) flash[i] = (uint8_t)(seed + i * 7u);
}

int main(void)
{
    NorbConfig c = cfg(), d;
    NorbLayout l;
    NorbHeader h, h2;
    uint8_t raw[NORB_HEADER_BYTES], saved[FLASH_SIZE], snapshot[FLASH_SIZE];
    uint32_t got, i;
    int which;
    long cut;

    /* the CRC-32 check value of the standard */
    CHECK(Norb_Crc32((const uint8_t *)"123456789", 9) == 0xCBF43926UL, "crc32 check value");
    CHECK(Norb_Crc32((const uint8_t *)"", 0) == 0UL, "crc32 of nothing");

    /* the defaults and the layout */
    Norb_Defaults(&d);
    CHECK(d.nor_offset == 0UL && d.nor_size == 16777216UL && d.slot_bytes == 786432UL, "defaults");
    CHECK(Norb_Plan(&d, &l) == NORB_OK, "plan of the defaults");
    CHECK(l.header_a == 0UL && l.header_b == 4096UL && l.image_offset == 65536UL && l.image_bytes_max == 786432UL && l.end == 65536UL + 786432UL, "layout of the defaults");
    CHECK(Norb_Plan(0, &l) == NORB_E_ARG && Norb_Plan(&d, 0) == NORB_E_ARG, "null arguments");
    d.slot_bytes = 0;
    CHECK(Norb_Plan(&d, &l) == NORB_E_ARG, "empty slot");
    Norb_Defaults(&d);
    d.nor_size = 0;
    CHECK(Norb_Plan(&d, &l) == NORB_E_ARG, "empty NOR");
    Norb_Defaults(&d);
    d.nor_offset = 4096UL;
    CHECK(Norb_Plan(&d, &l) == NORB_E_ALIGN, "offset not on a block");
    Norb_Defaults(&d);
    d.slot_bytes = 786432UL + 100UL;
    CHECK(Norb_Plan(&d, &l) == NORB_E_ALIGN, "slot not on a sector");
    Norb_Defaults(&d);
    d.nor_offset = 16777216UL - 65536UL;
    CHECK(Norb_Plan(&d, &l) == NORB_E_RANGE, "copy past the end");
    Norb_Defaults(&d);
    d.nor_offset = 2UL * 16777216UL;
    CHECK(Norb_Plan(&d, &l) == NORB_E_RANGE, "offset past the NOR");
    Norb_Defaults(&d);
    d.nor_size = 65536UL + 786432UL;
    CHECK(Norb_Plan(&d, &l) == NORB_OK, "copy that ends exactly at the end of the NOR");
    d.slot_bytes = 786432UL - 4096UL; /* rounds up to the same 12 blocks */
    CHECK(Norb_Plan(&d, &l) == NORB_OK && l.end == 65536UL + 786432UL, "a slot that is not a whole number of blocks takes the whole block");

    /* the header: literal bytes */
    h.sequence = 5;
    h.length = 0x1000;
    h.crc32 = 0xDEADBEEFUL;
    Norb_HeaderEncode(&h, raw);
    CHECK(raw[0] == 0x4E && raw[1] == 0x4F && raw[2] == 0x52 && raw[3] == 0x42, "magic NORB");
    CHECK(raw[4] == 5 && raw[8] == 0x00 && raw[9] == 0x10 && raw[12] == 0xEF && raw[15] == 0xDE && raw[16] == 0, "header fields little-endian");
    CHECK(Norb_Get32(raw + 20) == Norb_Crc32(raw, 20), "header crc");
    CHECK(Norb_HeaderDecode(raw, 0x2000UL, &h2) && h2.sequence == 5 && h2.length == 0x1000 && h2.crc32 == 0xDEADBEEFUL, "header round trip");
    CHECK(!Norb_HeaderDecode(raw, 0x800UL, &h2), "length above the slot");
    raw[0] ^= 1;
    CHECK(!Norb_HeaderDecode(raw, 0x2000UL, &h2), "damaged magic");
    raw[0] ^= 1;
    raw[5] ^= 1;
    CHECK(!Norb_HeaderDecode(raw, 0x2000UL, &h2), "damaged crc");
    h.length = 0;
    Norb_HeaderEncode(&h, raw);
    CHECK(!Norb_HeaderDecode(raw, 0x2000UL, &h2), "zero length");

    /* decide */
    CHECK(Norb_Decide(1, 1) == NORB_ACT_NONE && Norb_Decide(1, 0) == NORB_ACT_NONE, "a good application is left alone");
    CHECK(Norb_Decide(0, 1) == NORB_ACT_RESTORE, "a bad application with a copy is restored");
    CHECK(Norb_Decide(0, 0) == NORB_ACT_LOST, "a bad application without a copy is lost");

    /* nothing saved yet */
    reset();
    CHECK(Norb_Plan(&c, &l) == NORB_OK, "plan");
    CHECK(!Norb_ReadHeaders(&ops, &l, SLOT, &h, &which), "no header on an erased NOR");
    CHECK(!Norb_CopyExists(&ops, &c), "no copy on an erased NOR");
    CHECK(Norb_Restore(&ops, &c, &got) == NORB_E_NO_COPY, "restore without copy");
    d = c;
    d.nor_offset = 3;
    CHECK(!Norb_CopyExists(&ops, &d), "copy with a bad plan");
    CHECK(Norb_Restore(&ops, &d, &got) == NORB_E_ALIGN, "restore with a bad plan");
    CHECK(Norb_Save(&ops, &d, 100) == NORB_E_ALIGN, "save with a bad plan");
    CHECK(Norb_Save(&ops, &c, 0) == NORB_E_TOO_BIG && Norb_Save(&ops, &c, SLOT + 1) == NORB_E_TOO_BIG, "save of 0 or too many bytes");

    /* save then lose the application then restore */
    fill(3, 10000);
    memcpy(saved, flash, FLASH_SIZE);
    CHECK(Norb_Save(&ops, &c, 10000) == NORB_OK, "first save");
    CHECK(Norb_ReadHeaders(&ops, &l, SLOT, &h, &which) && which == 0 && h.sequence == 1 && h.length == 10000, "first save writes header A with sequence 1");
    CHECK(Norb_CopyExists(&ops, &c), "the copy exists");
    CHECK(Norb_Save(&ops, &c, 10000) == NORB_OK, "second save");
    CHECK(Norb_ReadHeaders(&ops, &l, SLOT, &h, &which) && which == 1 && h.sequence == 2, "second save writes header B with sequence 2");
    CHECK(Norb_Save(&ops, &c, 9000) == NORB_OK, "third save");
    CHECK(Norb_ReadHeaders(&ops, &l, SLOT, &h, &which) && which == 0 && h.sequence == 3 && h.length == 9000, "third save goes back to header A");
    reset();
    fill(3, 10000);
    memcpy(saved, flash, FLASH_SIZE);
    CHECK(Norb_Save(&ops, &c, 10000) == NORB_OK, "save again");
    memset(flash, 0x00, FLASH_SIZE);
    got = 0;
    CHECK(Norb_Restore(&ops, &c, &got) == NORB_OK && got == 10000, "restore");
    CHECK(memcmp(flash, saved, 10000) == 0, "the restored application is the saved one");
    CHECK(Norb_Restore(&ops, &c, 0) == NORB_OK, "restore without asking for the length");

    /* the sequence wraps: 0xFFFFFFFF then 0 */
    reset();
    h.sequence = 0xFFFFFFFFUL;
    h.length = 100;
    h.crc32 = 0;
    Norb_HeaderEncode(&h, raw);
    memcpy(nor + l.header_a, raw, NORB_HEADER_BYTES);
    h.sequence = 0;
    Norb_HeaderEncode(&h, raw);
    memcpy(nor + l.header_b, raw, NORB_HEADER_BYTES);
    CHECK(Norb_ReadHeaders(&ops, &l, SLOT, &h2, &which) && which == 1 && h2.sequence == 0, "the sequence that wrapped is the newer");

    /* one header damaged: the other is used; both damaged: no copy */
    reset();
    fill(9, 5000);
    CHECK(Norb_Save(&ops, &c, 5000) == NORB_OK && Norb_Save(&ops, &c, 5000) == NORB_OK, "two saves");
    nor[l.header_b] ^= 0xFF;
    CHECK(Norb_ReadHeaders(&ops, &l, SLOT, &h, &which) && which == 0 && h.sequence == 1, "header B damaged: A is used");
    nor[l.header_a] ^= 0xFF;
    CHECK(!Norb_ReadHeaders(&ops, &l, SLOT, &h, &which), "both damaged");
    reset();
    CHECK(Norb_Save(&ops, &c, 5000) == NORB_OK, "save");
    nor[l.header_a] ^= 0xFF;
    reset();
    fill(9, 5000);
    CHECK(Norb_Save(&ops, &c, 5000) == NORB_OK, "save");
    nor[l.header_b] = 0x00; /* only B damaged and A good, other order */
    CHECK(Norb_ReadHeaders(&ops, &l, SLOT, &h, &which) && which == 0, "only A good");
    reset();
    fill(9, 5000);
    CHECK(Norb_Save(&ops, &c, 5000) == NORB_OK && Norb_Save(&ops, &c, 5000) == NORB_OK, "two saves");
    nor[l.header_a] ^= 0xFF;
    CHECK(Norb_ReadHeaders(&ops, &l, SLOT, &h, &which) && which == 1, "only B good");

    /* a damaged image in the NOR is refused BEFORE the flash is touched */
    reset();
    fill(5, 8000);
    memcpy(saved, flash, FLASH_SIZE);
    CHECK(Norb_Save(&ops, &c, 8000) == NORB_OK, "save");
    nor[l.image_offset + 4000] ^= 0x10;
    CHECK(!Norb_CopyExists(&ops, &c), "a damaged image is not a copy");
    CHECK(Norb_Restore(&ops, &c, &got) == NORB_E_COPY_BAD, "restore refuses a damaged image");
    CHECK(memcmp(flash, saved, 8000) == 0, "the flash was not erased by the refused restore");

    /* failures of the memories */
    reset();
    fill(5, 8000);
    CHECK(Norb_Save(&ops, &c, 8000) == NORB_OK, "save");
    fail_nor_read_at = 0;
    nor_read_calls = 0;
    CHECK(Norb_Restore(&ops, &c, &got) == NORB_E_NO_COPY, "a header that cannot be read is no header");
    for (i = 0; i < 40; i++) {
        fail_nor_read_at = (int)i;
        nor_read_calls = 0;
        (void)Norb_Restore(&ops, &c, &got);
    }
    fail_nor_read_at = 2; /* the first read of the image, after both headers */
    nor_read_calls = 0;
    CHECK(Norb_Restore(&ops, &c, &got) == NORB_E_NOR_READ, "a failing NOR read in the check of the image");
    fail_nor_read_at = 2 + 32 + 1; /* the 8000 bytes take 32 reads to check; this is the first read of the copy to the flash */
    nor_read_calls = 0;
    CHECK(Norb_Restore(&ops, &c, &got) == NORB_E_NOR_READ, "a failing NOR read in the copy");
    fail_nor_read_at = -1;
    fail_flash_write_at = 3;
    flash_write_calls = 0;
    CHECK(Norb_Restore(&ops, &c, &got) == NORB_E_FLASH_WRITE, "a failing flash write");
    fail_flash_write_at = -1;
    fail_flash_read_at = 2;
    flash_read_calls = 0;
    CHECK(Norb_Restore(&ops, &c, &got) == NORB_E_FLASH_READ, "a failing flash read in the check");
    fail_flash_read_at = -1;
    flash_corrupt = 1;
    CHECK(Norb_Restore(&ops, &c, &got) == NORB_E_VERIFY, "what was written does not read back as the copy");
    flash_corrupt = 0;
    ops_left = 0;
    CHECK(Norb_Save(&ops, &c, 8000) == NORB_E_NOR_WRITE, "a NOR that refuses the erase");
    ops_left = 1;
    CHECK(Norb_Save(&ops, &c, 8000) == NORB_E_NOR_WRITE, "a NOR that refuses the first write");
    reset();
    fill(5, 8000);
    fail_flash_read_at = 0;
    flash_read_calls = 0;
    CHECK(Norb_Save(&ops, &c, 8000) == NORB_E_FLASH_READ, "a save from a flash that cannot be read");
    reset();
    fill(5, 8000);
    {
        long total;
        CHECK(Norb_Save(&ops, &c, 8000) == NORB_OK, "count the operations of a save");
        total = ops_done;
        CHECK(total > 10, "a save has several operations");
        /* a cut of the power (or a refusal) at every operation of a save, then a restore: the application of the flash is never touched, and a copy that is restored is an image that was saved */
        for (cut = 0; cut <= total; cut++) {
            reset();
            fill(5, 8000);
            memcpy(saved, flash, FLASH_SIZE);
            CHECK(Norb_Save(&ops, &c, 8000) == NORB_OK, "the first copy");
            fill(77, 6000);
            memcpy(snapshot, flash, FLASH_SIZE);
            ops_left = cut;
            (void)Norb_Save(&ops, &c, 6000);
            ops_left = -1;
            CHECK(memcmp(flash, snapshot, FLASH_SIZE) == 0, "flash untouched by a save");
            if (Norb_CopyExists(&ops, &c)) {
                uint8_t again[FLASH_SIZE];
                memcpy(again, flash, FLASH_SIZE);
                memset(flash, 0, FLASH_SIZE);
                CHECK(Norb_Restore(&ops, &c, &got) == NORB_OK, "a copy that exists restores");
                CHECK((got == 8000 && memcmp(flash, saved, 8000) == 0) || (got == 6000 && memcmp(flash, again, 6000) == 0), "the restored application is one that was saved");
            } else {
                CHECK(Norb_Restore(&ops, &c, &got) != NORB_OK, "no copy, no restore");
            }
        }
    }

    /* staging: an update that arrives page by page goes into the NOR, is checked there, and only then is it copied into the slot */
    {
        uint8_t update[9000], page[256];
        uint32_t offset, update_crc;
        for (i = 0; i < sizeof update; i++) update[i] = (uint8_t)(i * 13u + 5u);
        update_crc = Norb_Crc32(update, sizeof update);
        reset();
        fill(5, 8000);
        CHECK(Norb_Save(&ops, &c, 8000) == NORB_OK, "a copy of the running application");
        CHECK(Norb_StageBegin(&ops, &c, 0) == NORB_E_TOO_BIG && Norb_StageBegin(&ops, &c, SLOT + 1) == NORB_E_TOO_BIG, "an update of 0 or too many bytes");
        d = c;
        d.nor_offset = 3;
        CHECK(Norb_StageBegin(&ops, &d, 100) == NORB_E_ALIGN && Norb_StagePage(&ops, &d, 0, page, 1) == NORB_E_ALIGN && Norb_StageFinish(&ops, &d, 100, 0) == NORB_E_ALIGN, "a bad plan is refused by all three steps");
        CHECK(Norb_StageBegin(&ops, &c, sizeof update) == NORB_OK, "staging begins");
        CHECK(!Norb_CopyExists(&ops, &c), "while the update is half in, the older copy is no longer one");
        CHECK(Norb_StagePage(&ops, &c, 0, page, 0) == NORB_E_TOO_BIG && Norb_StagePage(&ops, &c, SLOT + 1, page, 1) == NORB_E_TOO_BIG && Norb_StagePage(&ops, &c, SLOT - 10, page, 11) == NORB_E_TOO_BIG, "a page outside the slot");
        for (offset = 0; offset < sizeof update; offset += 256) {
            uint32_t n = sizeof update - offset < 256 ? (uint32_t)sizeof update - offset : 256;
            memcpy(page, update + offset, n);
            CHECK(Norb_StagePage(&ops, &c, offset, page, n) == NORB_OK, "a page is staged");
        }
        CHECK(Norb_StageFinish(&ops, &c, sizeof update, update_crc ^ 1UL) == NORB_E_COPY_BAD, "a CRC the sender did not give is refused and no header is written");
        CHECK(!Norb_CopyExists(&ops, &c), "no copy before the check holds");
        CHECK(Norb_StageFinish(&ops, &c, 0, update_crc) == NORB_E_TOO_BIG, "an empty update");
        nor[Norb_Plan(&c, &l) == NORB_OK ? l.image_offset + 4000 : 0] ^= 0x01;
        CHECK(Norb_StageFinish(&ops, &c, sizeof update, update_crc) == NORB_E_COPY_BAD, "a damaged byte in the NOR is found");
        nor[l.image_offset + 4000] ^= 0x01;
        fail_nor_read_at = 2;
        nor_read_calls = 0;
        CHECK(Norb_StageFinish(&ops, &c, sizeof update, update_crc) == NORB_E_NOR_READ, "a failing NOR read in the check");
        fail_nor_read_at = -1;
        CHECK(Norb_StageFinish(&ops, &c, sizeof update, update_crc) == NORB_OK, "the update is complete");
        CHECK(Norb_CopyExists(&ops, &c), "the staged update is now a copy that can be restored");
        memset(flash, 0, FLASH_SIZE);
        got = 0;
        CHECK(Norb_Restore(&ops, &c, &got) == NORB_OK && got == sizeof update && memcmp(flash, update, sizeof update) == 0, "committing copies the staged update into the slot");
        /* a refusal of the NOR at each step */
        ops_left = 0;
        CHECK(Norb_StageBegin(&ops, &c, 100) == NORB_E_NOR_WRITE, "the NOR refuses the erase");
        CHECK(Norb_StagePage(&ops, &c, 0, page, 1) == NORB_E_NOR_WRITE, "the NOR refuses the page");
        ops_left = -1;
        /* a cut at every operation of the staging: whatever is left, a restore gives either nothing or an update that was complete */
        {
            long total;
            reset();
            CHECK(Norb_StageBegin(&ops, &c, sizeof update) == NORB_OK, "count the operations");
            for (offset = 0; offset < sizeof update; offset += 256) {
                uint32_t n = sizeof update - offset < 256 ? (uint32_t)sizeof update - offset : 256;
                memcpy(page, update + offset, n);
                CHECK(Norb_StagePage(&ops, &c, offset, page, n) == NORB_OK, "page");
            }
            CHECK(Norb_StageFinish(&ops, &c, sizeof update, update_crc) == NORB_OK, "finish");
            total = ops_done;
            for (long cut = 0; cut <= total; cut += 3) {
                reset();
                fill(5, 8000);
                ops_left = cut;
                if (Norb_StageBegin(&ops, &c, sizeof update) == NORB_OK) {
                    for (offset = 0; offset < sizeof update; offset += 256) {
                        uint32_t n = sizeof update - offset < 256 ? (uint32_t)sizeof update - offset : 256;
                        memcpy(page, update + offset, n);
                        if (Norb_StagePage(&ops, &c, offset, page, n) != NORB_OK) break;
                    }
                    (void)Norb_StageFinish(&ops, &c, sizeof update, update_crc);
                }
                ops_left = -1;
                if (Norb_CopyExists(&ops, &c)) {
                    memset(flash, 0, FLASH_SIZE);
                    CHECK(Norb_Restore(&ops, &c, &got) == NORB_OK && got == sizeof update && memcmp(flash, update, sizeof update) == 0, "only a complete staged update can be committed");
                }
            }
        }
    }

    if (failures) {
        printf("nor_backup_plan: %d FAILED\n", failures);
        return 1;
    }
    printf("nor_backup_plan host logic tests passed\n");
    return 0;
}
