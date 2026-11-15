/* =============================================================================
 * HYDRA-UMC Firmware - tests/test_boot_nor_stage.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Host test of the staging of an update in the NOR flash by the bootloader of the CM7 (src/mcu_stm32h757/Common/boot_nor_stage.h) against two RAM doubles (a NOR that can only clear
 * bits and an application slot): the pages of an update, the stream of the staged image to a consumer, the first words, the header written only when the CRC-32 matches, the commit over the
 * application, the refusal of a copy that is not the image the caller expects, the boot-time rule (nothing / restore / lost) and a cut of the power at every operation of a whole update:
 * the application is either the old image or the new one, and a cut after the header always leaves a copy that restores the new one. Evidence: simulator.
 *
 *   cc -std=c11 -Wall -Wextra -Werror -Isrc/mcu_stm32h757/Common -o test_boot_nor_stage tests/test_boot_nor_stage.c
 */
#include <stdio.h>
#include <string.h>

#include "boot_nor_stage.h"

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } } while (0)

#define NOR_SIZE (1024UL * 1024UL)
#define SLOT (16UL * 1024UL)
#define PAGE 2048UL

static uint8_t nor[NOR_SIZE];
static uint8_t flash[SLOT];
static long ops_left = -1; /* -1: never fail; otherwise every NOR/flash write operation fails once it reaches 0 */
static long ops_done;
static int fail_nor_read_at = -1, nor_read_calls;

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
    for (i = 0; i < n; i++) nor[o + i] &= d[i];
    return 0;
}
static int d_flash_read(void *ctx, uint32_t o, uint8_t *d, uint32_t n)
{
    (void)ctx;
    if (o + n > SLOT) return 1;
    memcpy(d, flash + o, n);
    return 0;
}
static int d_flash_erase(void *ctx, uint32_t n)
{
    (void)ctx;
    if (tick() || n > SLOT) return 1;
    memset(flash, 0xFF, SLOT);
    return 0;
}
static int d_flash_write(void *ctx, uint32_t o, const uint8_t *d, uint32_t n)
{
    (void)ctx;
    if (tick() || o + n > SLOT) return 1;
    memcpy(flash + o, d, n);
    return 0;
}

static const NorbOps ops = {d_nor_read, d_nor_erase, d_nor_write, d_flash_read, d_flash_erase, d_flash_write, 0};

static void reset(void)
{
    memset(nor, 0xFF, sizeof nor);
    memset(flash, 0xFF, sizeof flash);
    ops_left = -1;
    ops_done = 0;
    fail_nor_read_at = -1;
    nor_read_calls = 0;
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

static void image(uint8_t *out, uint8_t seed, uint32_t length)
{
    uint32_t i;
    for (i = 0; i < length; i++) out[i] = (uint8_t)(seed + i * 7u + (i >> 8));
}

/* the whole path of an update as the protocol code makes it: begin, a page at a time (the last one padded with 0xFF), finish; returns the first failing step's code */
static int stage_all(const BootNorStage *s, const uint8_t *img, uint32_t total, uint32_t crc)
{
    uint8_t page[PAGE];
    uint32_t index = 0, done = 0, n;
    int rc = BootNorStage_Begin(s, total);
    if (rc != NORB_OK) return rc;
    while (done < total) {
        n = (total - done < PAGE) ? total - done : PAGE;
        memset(page, 0xFF, sizeof page);
        memcpy(page, img + done, n);
        rc = BootNorStage_Page(s, index++, PAGE, page, PAGE);
        if (rc != NORB_OK) return rc;
        done += n;
    }
    return BootNorStage_Finish(s, total, crc);
}

typedef struct {
    uint32_t seen;
    uint32_t crc;
    int refuse_after;
} Sink;

static int sink_fn(void *ctx, const uint8_t *data, uint32_t n)
{
    Sink *k = (Sink *)ctx;
    uint32_t i;
    if (k->refuse_after >= 0 && (int)k->seen >= k->refuse_after) return 1;
    for (i = 0; i < n; i++) k->crc += data[i];
    k->seen += n;
    return 0;
}

int main(void)
{
    BootNorStage s;
    NorbConfig c = cfg();
    uint8_t a[SLOT], b[SLOT], back[SLOT], page[PAGE];
    uint32_t total = 5000UL, crc_a, crc_b, got, i, sum = 0;
    int action = -1, rc;
    long cut, whole;
    Sink k;

    image(a, 0x11, SLOT);
    image(b, 0x77, SLOT);
    crc_a = Norb_Crc32(a, total);
    crc_b = Norb_Crc32(b, total);
    BootNorStage_Init(&s, &c, &ops);

    /* null arguments are refused everywhere */
    CHECK(BootNorStage_Begin(0, total) == BNS_E_ARG, "begin null");
    CHECK(BootNorStage_Page(0, 0, PAGE, page, PAGE) == BNS_E_ARG, "page null stage");
    CHECK(BootNorStage_Page(&s, 0, PAGE, 0, PAGE) == BNS_E_ARG, "page null data");
    CHECK(BootNorStage_Page(&s, 0, 0, page, PAGE) == BNS_E_ARG, "page of no bytes");
    CHECK(BootNorStage_Page(&s, SLOT / PAGE + 1UL, PAGE, page, PAGE) == BNS_E_ARG, "page index far outside");
    CHECK(BootNorStage_Read(0, 0, page, 4) == BNS_E_ARG && BootNorStage_Read(&s, 0, 0, 4) == BNS_E_ARG, "read null");
    CHECK(BootNorStage_Read(&s, 0, page, 0) == BNS_E_ARG, "read of nothing");
    CHECK(BootNorStage_Read(&s, SLOT + 1UL, page, 4) == BNS_E_ARG && BootNorStage_Read(&s, SLOT - 2UL, page, 4) == BNS_E_ARG, "read outside the slot");
    CHECK(BootNorStage_Stream(&s, total, 0, 0) == BNS_E_ARG, "stream without a sink");
    CHECK(BootNorStage_Finish(0, total, 0) == BNS_E_ARG, "finish null");
    CHECK(BootNorStage_CopyIs(0, total, 0) == 0, "copy-is null");
    CHECK(BootNorStage_Commit(0, total, 0) == BNS_E_ARG, "commit null");
    CHECK(BootNorStage_Recover(0, 0, total, 0, &action) == BNS_E_ARG && BootNorStage_Recover(&s, 0, total, 0, 0) == BNS_E_ARG, "recover null");

    /* a configuration the plan refuses is refused by every step */
    {
        BootNorStage bad = s;
        bad.cfg.nor_offset = 4096UL;
        CHECK(BootNorStage_Read(&bad, 0, page, 4) == NORB_E_ALIGN, "read with a bad plan");
        CHECK(BootNorStage_CopyIs(&bad, total, crc_a) == 0, "copy-is with a bad plan");
    }

    /* nothing staged: no copy, nothing to commit, the rule says lost for a bad application and nothing for a good one */
    reset();
    CHECK(BootNorStage_CopyIs(&s, total, crc_a) == 0, "no copy yet");
    CHECK(BootNorStage_Commit(&s, total, crc_a) == BNS_E_MISMATCH, "no commit without a copy");
    CHECK(BootNorStage_Recover(&s, 1, total, crc_a, &action) == BNS_OK && action == BNS_RECOVER_NONE, "good application: nothing");
    CHECK(BootNorStage_Recover(&s, 0, total, crc_a, &action) == BNS_OK && action == BNS_RECOVER_LOST, "bad application and no copy: lost");

    /* a whole update: stage, read, stream, commit */
    CHECK(stage_all(&s, a, total, crc_a) == NORB_OK, "stage a whole update");
    CHECK(BootNorStage_CopyIs(&s, total, crc_a), "the copy is the staged image");
    CHECK(!BootNorStage_CopyIs(&s, total, crc_a ^ 1UL) && !BootNorStage_CopyIs(&s, total - 1UL, crc_a), "the copy is not another length or CRC-32");
    CHECK(BootNorStage_Read(&s, 0, back, 8) == NORB_OK && memcmp(back, a, 8) == 0, "first words read back");
    CHECK(BootNorStage_Read(&s, total - 4UL, back, 4) == NORB_OK && memcmp(back, a + total - 4UL, 4) == 0, "last bytes read back");
    memset(&k, 0, sizeof k);
    k.refuse_after = -1;
    CHECK(BootNorStage_Stream(&s, total, sink_fn, &k) == BNS_OK && k.seen == total, "stream delivers every byte");
    for (i = 0; i < total; i++) sum += a[i];
    CHECK(k.crc == sum, "stream delivers the bytes of the image");
    k.seen = 0;
    k.refuse_after = 300;
    CHECK(BootNorStage_Stream(&s, total, sink_fn, &k) == BNS_E_SINK, "a sink that refuses stops the stream");
    fail_nor_read_at = 1;
    nor_read_calls = 0;
    k.seen = 0;
    k.refuse_after = -1;
    CHECK(BootNorStage_Stream(&s, total, sink_fn, &k) == NORB_E_NOR_READ, "an unreadable NOR stops the stream");
    fail_nor_read_at = -1;
    CHECK(BootNorStage_Stream(&s, 0, sink_fn, &k) == BNS_OK, "a stream of nothing is empty");

    CHECK(BootNorStage_Commit(&s, total, crc_b) == BNS_E_MISMATCH, "a commit of another CRC-32 is refused");
    CHECK(flash[0] == 0xFF, "a refused commit leaves the flash alone");
    CHECK(BootNorStage_Commit(&s, total, crc_a) == BNS_OK, "commit");
    CHECK(memcmp(flash, a, total) == 0, "the application is the staged image");

    /* the boot-time rule over a damaged application */
    flash[10] ^= 0x40u;
    CHECK(BootNorStage_Recover(&s, 1, total, crc_a, &action) == BNS_OK && action == BNS_RECOVER_NONE && flash[10] != a[10], "good application: left as it is");
    CHECK(BootNorStage_Recover(&s, 0, total, crc_a, &action) == BNS_OK && action == BNS_RECOVER_DONE && memcmp(flash, a, total) == 0, "damaged application: restored");
    flash[10] ^= 0x40u;
    CHECK(BootNorStage_Recover(&s, 0, total, crc_b, &action) == BNS_OK && action == BNS_RECOVER_LOST && flash[10] != a[10], "a copy that is not the image of the metadata is not used");
    CHECK(BootNorStage_Recover(&s, 0, total + 1UL, crc_a, &action) == BNS_OK && action == BNS_RECOVER_LOST, "nor a copy of another length");

    /* a copy whose image was damaged after the header is refused too */
    nor[131072UL + 100UL] = 0x00u; /* byte 100 of A is not 0 */
    CHECK(BootNorStage_Recover(&s, 0, total, crc_a, &action) == BNS_OK && action == BNS_RECOVER_LOST, "a damaged image in the NOR is lost, not restored");
    CHECK(BootNorStage_Commit(&s, total, crc_a) == NORB_E_COPY_BAD, "and a commit of it is refused");

    /* a restore that fails is reported (the flash refuses the write) */
    reset();
    CHECK(stage_all(&s, a, total, crc_a) == NORB_OK, "stage again");
    ops_left = 1;
    rc = BootNorStage_Recover(&s, 0, total, crc_a, &action);
    CHECK(rc != BNS_OK && action == BNS_RECOVER_DONE, "a failing restore is reported");
    ops_left = -1;

    /* finish refuses a staged image whose CRC-32 is not the one given, and writes no header */
    reset();
    CHECK(BootNorStage_Begin(&s, total) == NORB_OK, "begin");
    memset(page, 0xFF, sizeof page);
    memcpy(page, a, PAGE);
    CHECK(BootNorStage_Page(&s, 0, PAGE, page, PAGE) == NORB_OK, "one page");
    CHECK(BootNorStage_Finish(&s, total, crc_a) == NORB_E_COPY_BAD, "an incomplete image is not finished");
    CHECK(!BootNorStage_CopyIs(&s, total, crc_a), "no header was written");
    CHECK(BootNorStage_Begin(&s, SLOT + 1UL) == NORB_E_TOO_BIG && BootNorStage_Begin(&s, 0) == NORB_E_TOO_BIG, "begin refuses a size the slot cannot hold");

    /* the page that ends exactly at the slot is accepted, one more is not */
    reset();
    CHECK(BootNorStage_Begin(&s, SLOT) == NORB_OK, "begin of a full slot");
    CHECK(BootNorStage_Page(&s, SLOT / PAGE - 1UL, PAGE, page, PAGE) == NORB_OK, "last page of the slot");
    CHECK(BootNorStage_Page(&s, SLOT / PAGE, PAGE, page, PAGE) == NORB_E_TOO_BIG, "a page past the slot");

    /* a cut of the power at every NOR/flash operation of "stage B over an installed A, then commit": the application is A or B, and once the header of B is there it restores B */
    reset();
    CHECK(stage_all(&s, a, total, crc_a) == NORB_OK && BootNorStage_Commit(&s, total, crc_a) == BNS_OK, "A is installed");
    ops_done = 0;
    CHECK(stage_all(&s, b, total, crc_b) == NORB_OK && BootNorStage_Commit(&s, total, crc_b) == BNS_OK, "B goes through whole");
    whole = ops_done;
    CHECK(whole > 5, "the whole update has operations to cut");
    for (cut = 0; cut < whole; cut++) {
        int staged;
        reset();
        CHECK(stage_all(&s, a, total, crc_a) == NORB_OK && BootNorStage_Commit(&s, total, crc_a) == BNS_OK, "A installed for the cut");
        ops_done = 0;
        ops_left = cut;
        staged = stage_all(&s, b, total, crc_b) == NORB_OK;
        if (staged) staged = BootNorStage_Commit(&s, total, crc_b) == BNS_OK;
        ops_left = -1;
        got = BootNorStage_CopyIs(&s, total, crc_b);
        if (!staged && !got) {
            /* the cut came before the header of B: the application was never touched */
            CHECK(memcmp(flash, a, total) == 0, "a cut before the header leaves the application alone");
        } else if (!staged && got) {
            /* the cut came after the header of B: the application is A, or half of B, and the copy restores B */
            rc = BootNorStage_Recover(&s, memcmp(flash, a, total) == 0, total, crc_b, &action);
            CHECK(rc == BNS_OK, "the recovery after a cut works");
            if (memcmp(flash, a, total) != 0) CHECK(action == BNS_RECOVER_DONE && memcmp(flash, b, total) == 0, "a cut inside the commit is repaired with B");
        }
    }
    (void)got;

    if (failures == 0) printf("boot_nor_stage host logic tests passed\n");
    return failures == 0 ? 0 : 1;
}
