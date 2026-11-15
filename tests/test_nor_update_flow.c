/* =============================================================================
 * HYDRA-UMC Firmware - tests/test_nor_update_flow.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Host test of the backup of the application slot before an update (src/mcu_stm32h757/Common/nor_update_flow.h) against two RAM doubles (a NOR with erase-before-write and a flash): the
 * layout of the two regions and its refusals, the backup only when the application is good, the update refused when the backup fails and allowed when the file says so, the start-up
 * choice in all its branches, the way back to the previous application when the new one is lost, a cut of the power at every operation of the backup, after which the running
 * application is untouched and a previous copy that is restored is always an application that was saved, the start-up choice made against the metadata of the application
 * (Nuf_RecoverFor), the version of the saved application kept in the header of the previous copy (and a copy without one reading as 0.0.0), and a cut of the power at every operation of a whole update (backup, staging, finish, commit), after which one of the two images is always in the flash. Evidence: simulator.
 *
 *   cc -std=c11 -Wall -Wextra -Werror -Isrc/mcu_stm32h757/Common -o test_nor_update_flow tests/test_nor_update_flow.c
 */
#include <stdio.h>
#include <string.h>

#include "nor_update_flow.h"

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } } while (0)

#define NOR_SIZE (1024UL * 1024UL)
#define SLOT (16UL * 1024UL)

static uint8_t nor[NOR_SIZE];
static uint8_t flash[SLOT];
static long ops_left = -1; /* -1: never fail; otherwise fail every write or erase once it reaches 0 */
static long ops_done;
static int flash_erase_fails, flash_write_fails_once;

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
    if (tick() || flash_erase_fails || n > SLOT) return 1;
    memset(flash, 0xFF, SLOT);
    return 0;
}
static int d_flash_write(void *ctx, uint32_t o, const uint8_t *d, uint32_t n)
{
    (void)ctx;
    if (tick()) return 1;
    if (flash_write_fails_once) {
        flash_write_fails_once = 0;
        return 1;
    }
    if (o + n > SLOT) return 1;
    memcpy(flash + o, d, n);
    return 0;
}

static NorbOps ops = {d_nor_read, d_nor_erase, d_nor_write, d_flash_read, d_flash_erase, d_flash_write, 0};

static void reset(void)
{
    memset(nor, 0xFF, sizeof nor);
    memset(flash, 0xFF, sizeof flash);
    ops_left = -1;
    ops_done = 0;
    flash_erase_fails = flash_write_fails_once = 0;
}

static void fill(uint8_t seed, uint32_t length)
{
    uint32_t i;
    for (i = 0; i < length; i++) flash[i] = (uint8_t)(seed + i * 7u);
}

/* the current region at 0 and the previous one right after it: 64 KB of headers and 64 KB of image (a 16 KB slot takes a whole block) each */
static NufConfig cfg(void)
{
    NufConfig c;
    Nuf_Defaults(&c);
    c.current.nor_size = NOR_SIZE;
    c.current.slot_bytes = SLOT;
    c.previous.nor_size = NOR_SIZE;
    c.previous.slot_bytes = SLOT;
    c.previous.nor_offset = 131072UL;
    return c;
}

int main(void)
{
    NufConfig c = cfg(), d;
    NorbLayout lc, lp;
    uint8_t old_app[SLOT], new_app[SLOT], snapshot[SLOT];
    uint32_t got = 0;
    int allowed, used;
    long total, cut;

    /* the defaults: previous region right after the current one, a backup is required */
    Nuf_Defaults(&d);
    CHECK(d.current.nor_offset == 0UL && d.previous.nor_offset == NUF_DEFAULT_PREVIOUS_OFFSET && d.require_backup == 1UL, "defaults");
    CHECK(Nuf_Plan(&d, &lc, &lp) == NUF_OK && lc.end == NUF_DEFAULT_PREVIOUS_OFFSET && lp.header_a == lc.end, "the default regions touch without overlapping");
    CHECK(lp.end <= d.previous.nor_size, "and both fit the NOR");

    /* the layout and its refusals */
    CHECK(Nuf_Plan(0, &lc, &lp) == NUF_E_ARG && Nuf_Plan(&d, 0, &lp) == NUF_E_ARG && Nuf_Plan(&d, &lc, 0) == NUF_E_ARG, "null arguments");
    d = c;
    d.previous.nor_offset = 65536UL;
    CHECK(Nuf_Plan(&d, &lc, &lp) == NUF_E_LAYOUT, "the regions overlap");
    d.previous.nor_offset = 0UL;
    CHECK(Nuf_Plan(&d, &lc, &lp) == NUF_E_LAYOUT, "the same region twice");
    d = c;
    d.current.nor_offset = 131072UL;
    d.previous.nor_offset = 0UL;
    CHECK(Nuf_Plan(&d, &lc, &lp) == NUF_OK, "the previous region may come first");
    d = c;
    d.previous.nor_offset = 3UL;
    CHECK(Nuf_Plan(&d, &lc, &lp) == NUF_E_LAYOUT, "a previous region off the block");
    d = c;
    d.current.nor_offset = 3UL;
    CHECK(Nuf_Plan(&d, &lc, &lp) == NUF_E_LAYOUT, "a current region off the block");
    d = c;
    d.previous.nor_offset = NOR_SIZE - 65536UL;
    CHECK(Nuf_Plan(&d, &lc, &lp) == NUF_E_LAYOUT, "a previous region past the end of the NOR");
    d = c;
    d.previous.slot_bytes = SLOT - 4096UL;
    CHECK(Nuf_Plan(&d, &lc, &lp) == NUF_E_LAYOUT, "two different slots");

    /* decide */
    CHECK(Nuf_Decide(1, 1, 1) == NUF_ACT_NONE && Nuf_Decide(1, 0, 0) == NUF_ACT_NONE, "a good application is left alone");
    CHECK(Nuf_Decide(0, 1, 0) == NUF_ACT_RESTORE_CURRENT && Nuf_Decide(0, 1, 1) == NUF_ACT_RESTORE_CURRENT, "the current copy comes first");
    CHECK(Nuf_Decide(0, 0, 1) == NUF_ACT_ROLLBACK_PREVIOUS, "then the previous one");
    CHECK(Nuf_Decide(0, 0, 0) == NUF_ACT_LOST, "then nothing");

    /* the backup: arguments and layout */
    reset();
    fill(3, 10000);
    CHECK(Nuf_BackupBeforeUpdate(&ops, &c, 1, 10000, 0UL, 0) == NUF_E_ARG, "no answer pointer");
    allowed = 5;
    CHECK(Nuf_BackupBeforeUpdate(0, &c, 1, 10000, 0UL, &allowed) == NUF_E_ARG && allowed == 0, "no operations");
    d = c;
    d.previous.nor_offset = 0UL;
    CHECK(Nuf_BackupBeforeUpdate(&ops, &d, 1, 10000, 0UL, &allowed) == NUF_E_LAYOUT && allowed == 0, "a bad layout refuses the update");
    CHECK(Nuf_BackupBeforeUpdate(&ops, 0, 1, 10000, 0UL, &allowed) == NUF_E_ARG && allowed == 0, "no configuration");

    /* a broken application is not backed up and the update is its repair */
    reset();
    CHECK(Nuf_BackupBeforeUpdate(&ops, &c, 0, 10000, 0UL, &allowed) == NUF_SKIPPED && allowed == 1, "nothing to protect");
    CHECK(ops_done == 0, "and nothing written");

    /* a good application is saved into the previous region, not the current one */
    reset();
    fill(3, 10000);
    memcpy(old_app, flash, SLOT);
    CHECK(Nuf_BackupBeforeUpdate(&ops, &c, 1, 10000, 0UL, &allowed) == NUF_OK && allowed == 1, "backup");
    CHECK(Norb_CopyExists(&ops, &c.previous) && !Norb_CopyExists(&ops, &c.current), "the copy is in the previous region only");
    CHECK(Nuf_Plan(&c, &lc, &lp) == NUF_OK && memcmp(nor + lp.image_offset, old_app, 10000) == 0, "and it is the running application");

    /* a backup that fails: refused when required, allowed when the file says so */
    reset();
    fill(3, 10000);
    ops_left = 0;
    CHECK(Nuf_BackupBeforeUpdate(&ops, &c, 1, 10000, 0UL, &allowed) == NUF_E_BACKUP && allowed == 0, "required backup that fails refuses the update");
    d = c;
    d.require_backup = 0UL;
    ops_left = 0;
    CHECK(Nuf_BackupBeforeUpdate(&ops, &d, 1, 10000, 0UL, &allowed) == NUF_E_BACKUP && allowed == 1, "optional backup that fails lets it go on");
    ops_left = -1;

    /* a full update: back up, stage, commit; then the new application is lost and the way back is the old one */
    reset();
    fill(3, 10000);
    memcpy(old_app, flash, SLOT);
    CHECK(Nuf_BackupBeforeUpdate(&ops, &c, 1, 10000, 0UL, &allowed) == NUF_OK && allowed == 1, "backup of the old application");
    fill(77, 12000);
    memcpy(new_app, flash, SLOT);
    CHECK(Norb_Save(&ops, &c.current, 12000) == NORB_OK, "the new image in the current region (what staging ends with)");
    CHECK(Nuf_Recover(&ops, &c, 1, &got, &used) == NUF_OK && used == NUF_ACT_NONE, "a good new application: nothing to do");
    memset(flash, 0x00, SLOT);
    CHECK(Nuf_Recover(&ops, &c, 0, &got, &used) == NUF_OK && used == NUF_ACT_RESTORE_CURRENT && got == 12000, "a cut in the commit: the current copy is restored");
    CHECK(memcmp(flash, new_app, 12000) == 0, "and it is the new application");
    /* the current copy is damaged: the previous application is the way back */
    CHECK(Nuf_Plan(&c, &lc, &lp) == NUF_OK, "layout");
    nor[lc.image_offset + 100] ^= 0x10;
    memset(flash, 0x00, SLOT);
    CHECK(Nuf_Recover(&ops, &c, 0, &got, &used) == NUF_OK && used == NUF_ACT_ROLLBACK_PREVIOUS && got == 10000, "a damaged current copy falls back to the previous application");
    CHECK(memcmp(flash, old_app, 10000) == 0, "and it is the old application");
    /* the current copy is good but the flash refuses its first write: the previous application is tried next */
    nor[lc.image_offset + 100] ^= 0x10;
    flash_write_fails_once = 1;
    CHECK(Nuf_Recover(&ops, &c, 0, &got, &used) == NUF_OK && used == NUF_ACT_ROLLBACK_PREVIOUS && got == 10000, "a restore that fails falls back to the previous application");

    /* only the previous copy exists */
    reset();
    fill(3, 10000);
    memcpy(old_app, flash, SLOT);
    CHECK(Norb_Save(&ops, &c.previous, 10000) == NORB_OK, "previous only");
    memset(flash, 0x00, SLOT);
    CHECK(Nuf_Recover(&ops, &c, 0, 0, &used) == NUF_OK && used == NUF_ACT_ROLLBACK_PREVIOUS && memcmp(flash, old_app, 10000) == 0, "the previous application comes back");

    /* nothing to put back, or a copy that cannot be written */
    reset();
    CHECK(Nuf_Recover(&ops, &c, 0, &got, &used) == NUF_E_NONE && used == NUF_ACT_LOST, "no copy at all");
    CHECK(Nuf_Recover(0, &c, 0, &got, &used) == NUF_E_ARG && Nuf_Recover(&ops, &c, 0, &got, 0) == NUF_E_ARG, "null arguments of the recovery");
    d = c;
    d.previous.nor_offset = 0UL;
    CHECK(Nuf_Recover(&ops, &d, 0, &got, &used) == NUF_E_LAYOUT, "a bad layout stops the recovery");
    fill(3, 10000);
    CHECK(Norb_Save(&ops, &c.previous, 10000) == NORB_OK, "previous copy");
    /* a flash that refuses the erase: no copy can be put back and the application is lost */
    flash_erase_fails = 1;
    CHECK(Nuf_Recover(&ops, &c, 0, &got, &used) == NUF_E_NONE && used == NUF_ACT_LOST, "a flash that cannot be erased loses the application");
    flash_erase_fails = 0;

    /* a cut of the power at every operation of the backup: the running application is untouched, and a previous copy that is restored is one that was saved */
    reset();
    fill(3, 10000);
    CHECK(Nuf_BackupBeforeUpdate(&ops, &c, 1, 10000, 0UL, &allowed) == NUF_OK, "count the operations of a backup");
    total = ops_done;
    CHECK(total > 10, "a backup has several operations");
    for (cut = 0; cut <= total; cut++) {
        reset();
        fill(3, 10000);
        memcpy(old_app, flash, SLOT);
        CHECK(Nuf_BackupBeforeUpdate(&ops, &c, 1, 10000, 0UL, &allowed) == NUF_OK, "the first backup");
        fill(77, 6000);
        memcpy(snapshot, flash, SLOT);
        ops_left = cut;
        (void)Nuf_BackupBeforeUpdate(&ops, &c, 1, 6000, 0UL, &allowed);
        ops_left = -1;
        CHECK(memcmp(flash, snapshot, SLOT) == 0, "the running application is untouched by a backup");
        if (Norb_CopyExists(&ops, &c.previous)) {
            memset(flash, 0x00, SLOT);
            CHECK(Nuf_Recover(&ops, &c, 0, &got, &used) == NUF_OK && used == NUF_ACT_ROLLBACK_PREVIOUS, "a surviving previous copy restores");
            CHECK((got == 10000 && memcmp(flash, old_app, 10000) == 0) || (got == 6000 && memcmp(flash, snapshot, 6000) == 0), "and it is an application that was saved");
        }
    }

    /* the version of the application travels in the header of the previous copy */
    {
        NufRestored r;
        uint32_t ma, mi, pa, v1 = Nuf_VersionPack(0, 9, 4), v2 = Nuf_VersionPack(1, 20, 1023);
        uint8_t raw[NORB_HEADER_BYTES];
        NorbHeader h;
        int which;
        CHECK(Nuf_VersionPack(0, 0, 0) == 0UL && v1 == ((9UL << 10) | 4UL) && v2 == ((1UL << 20) | (20UL << 10) | 1023UL), "the packed version");
        Nuf_VersionUnpack(v2, &ma, &mi, &pa);
        CHECK(ma == 1UL && mi == 20UL && pa == 1023UL, "and it unpacks");
        CHECK(Nuf_VersionPack(1024UL, 0, 0) == 0UL && Nuf_VersionPack(0, 1024UL, 0) == 0UL && Nuf_VersionPack(0, 0, 1024UL) == 0UL, "a number that does not fit reads as not known");
        Nuf_VersionUnpack(0x40000000UL | v1, &ma, &mi, &pa);
        CHECK(ma == 0UL && mi == 0UL && pa == 0UL, "a word with bits outside the three fields reads as 0.0.0");

        /* back up with a version: the header of the previous copy keeps it and the way back returns it */
        reset();
        fill(3, 10000);
        memcpy(old_app, flash, SLOT);
        CHECK(Nuf_BackupBeforeUpdate(&ops, &c, 1, 10000, v1, &allowed) == NUF_OK && allowed == 1, "backup with a version");
        CHECK(Nuf_Plan(&c, &lc, &lp) == NUF_OK, "layout");
        CHECK(Norb_ReadHeaders(&ops, &lp, SLOT, &h, &which) && h.version == v1 && h.length == 10000UL, "the header of the previous copy keeps the version");
        memcpy(raw, nor + lp.header_a, NORB_HEADER_BYTES);
        CHECK(Norb_Get32(raw + 16) == v1, "in the word that older copies keep at zero");
        memset(flash, 0x00, SLOT);
        CHECK(Nuf_RecoverFor(&ops, &c, 0, 5UL, 6UL, &r, &used) == NUF_OK && used == NUF_ACT_ROLLBACK_PREVIOUS && r.length == 10000UL && r.version == v1, "the way back returns the version");
        CHECK(memcmp(flash, old_app, 10000) == 0, "with the application");

        /* a second backup replaces the version along with the image */
        fill(77, 6000);
        CHECK(Nuf_BackupBeforeUpdate(&ops, &c, 1, 6000, v2, &allowed) == NUF_OK, "second backup");
        memset(flash, 0x00, SLOT);
        CHECK(Nuf_RecoverFor(&ops, &c, 0, 5UL, 6UL, &r, &used) == NUF_OK && r.length == 6000UL && r.version == v2, "the newest version");

        /* a copy made before the version was kept (a header with the zero word) comes back as 0.0.0, and the restore keeps working */
        reset();
        fill(3, 10000);
        CHECK(Norb_Save(&ops, &c.previous, 10000) == NORB_OK, "a copy without a version");
        r.version = 77UL;
        memset(flash, 0x00, SLOT);
        CHECK(Nuf_RecoverFor(&ops, &c, 0, 5UL, 6UL, &r, &used) == NUF_OK && used == NUF_ACT_ROLLBACK_PREVIOUS && r.version == 0UL && r.length == 10000UL, "an old copy reads as 0.0.0");
        CHECK(Nuf_VersionPack(0, 0, 0) == 0UL, "which is the value of no version");

        /* a lost application and a bad argument leave no stale version behind */
        reset();
        r.version = 77UL;
        CHECK(Nuf_RecoverFor(&ops, &c, 0, 5UL, 6UL, &r, &used) == NUF_E_NONE && used == NUF_ACT_LOST && r.version == 0UL, "nothing to put back: no version");

        /* a cut of the power at every operation of a second backup: the version that comes back always belongs to the length that comes back */
        reset();
        fill(3, 10000);
        CHECK(Nuf_BackupBeforeUpdate(&ops, &c, 1, 10000, v1, &allowed) == NUF_OK, "first backup");
        fill(77, 6000);
        CHECK(Nuf_BackupBeforeUpdate(&ops, &c, 1, 6000, v2, &allowed) == NUF_OK, "count the operations of the second backup");
        total = ops_done;
        for (cut = 0; cut <= total; cut++) {
            reset();
            fill(3, 10000);
            CHECK(Nuf_BackupBeforeUpdate(&ops, &c, 1, 10000, v1, &allowed) == NUF_OK, "the first backup");
            fill(77, 6000);
            memcpy(snapshot, flash, SLOT);
            ops_left = cut;
            (void)Nuf_BackupBeforeUpdate(&ops, &c, 1, 6000, v2, &allowed);
            ops_left = -1;
            memset(flash, 0x00, SLOT);
            if (Nuf_RecoverFor(&ops, &c, 0, 5UL, 6UL, &r, &used) == NUF_OK) {
                CHECK((r.length == 10000UL && r.version == v1) || (r.length == 6000UL && r.version == v2), "after a cut the version is the one of the application that came back");
            }
        }
    }

    /* the start-up choice against the metadata: arguments, layout, a good application */
    {
        NufRestored r;
        uint32_t old_crc, new_crc;
        reset();
        fill(3, 10000);
        memcpy(old_app, flash, SLOT);
        old_crc = Norb_Crc32(old_app, 10000);
        CHECK(Nuf_RecoverFor(0, &c, 0, 10000, old_crc, &r, &used) == NUF_E_ARG && Nuf_RecoverFor(&ops, &c, 0, 10000, old_crc, 0, &used) == NUF_E_ARG && Nuf_RecoverFor(&ops, &c, 0, 10000, old_crc, &r, 0) == NUF_E_ARG, "null arguments of the recovery against the metadata");
        d = c;
        d.previous.nor_offset = 0UL;
        CHECK(Nuf_RecoverFor(&ops, &d, 0, 10000, old_crc, &r, &used) == NUF_E_LAYOUT && used == NUF_ACT_NONE, "a bad layout stops it");
        CHECK(Nuf_RecoverFor(&ops, &c, 1, 10000, old_crc, &r, &used) == NUF_OK && used == NUF_ACT_NONE, "a good application is left alone");
        CHECK(Nuf_RecoverFor(&ops, &c, 0, 10000, old_crc, &r, &used) == NUF_E_NONE && used == NUF_ACT_LOST, "no copy at all");

        /* the current copy is the image of the metadata: it comes back */
        CHECK(Norb_Save(&ops, &c.current, 10000) == NORB_OK, "current copy");
        memset(flash, 0x00, SLOT);
        CHECK(Nuf_RecoverFor(&ops, &c, 0, 10000, old_crc, &r, &used) == NUF_OK && used == NUF_ACT_RESTORE_CURRENT && r.length == 10000 && r.crc32 == old_crc, "the current copy comes back");
        CHECK(memcmp(flash, old_app, 10000) == 0, "and it is the image of the metadata");

        /* the current copy is another image than the metadata describes (a different CRC-32, a different length): it is not used and, with no previous copy, the application is lost */
        memset(flash, 0x00, SLOT);
        CHECK(Nuf_RecoverFor(&ops, &c, 0, 10000, old_crc ^ 1UL, &r, &used) == NUF_E_NONE && used == NUF_ACT_LOST, "an older copy never replaces the application of the metadata");
        CHECK(Nuf_RecoverFor(&ops, &c, 0, 9999, old_crc, &r, &used) == NUF_E_NONE && used == NUF_ACT_LOST, "nor one of another length");
        CHECK(flash[0] == 0x00, "and the flash was left as it was");

        /* a previous copy takes over from a current copy that does not match: its length and CRC-32 come back for the new metadata */
        fill(77, 12000);
        new_crc = Norb_Crc32(flash, 12000);
        memcpy(new_app, flash, SLOT);
        CHECK(Norb_Save(&ops, &c.previous, 12000) == NORB_OK, "previous copy");
        memset(flash, 0x00, SLOT);
        CHECK(Nuf_RecoverFor(&ops, &c, 0, 10000, old_crc ^ 1UL, &r, &used) == NUF_OK && used == NUF_ACT_ROLLBACK_PREVIOUS && r.length == 12000 && r.crc32 == new_crc, "the previous application comes back and says what it is");
        CHECK(memcmp(flash, new_app, 12000) == 0, "and it is that application");
        /* the current copy matches but the flash refuses the first write: the previous application is next */
        memset(flash, 0x00, SLOT);
        flash_write_fails_once = 1;
        CHECK(Nuf_RecoverFor(&ops, &c, 0, 10000, old_crc, &r, &used) == NUF_OK && used == NUF_ACT_ROLLBACK_PREVIOUS && r.length == 12000, "a restore that fails falls back to the previous application");
        /* neither can be written: lost */
        flash_erase_fails = 1;
        CHECK(Nuf_RecoverFor(&ops, &c, 0, 10000, old_crc, &r, &used) == NUF_E_NONE && used == NUF_ACT_LOST, "a flash that cannot be erased loses the application");
        flash_erase_fails = 0;
        /* a current copy whose image was damaged after its header is not a copy */
        nor[65536UL + 100UL] ^= 0x10;
        memset(flash, 0x00, SLOT);
        CHECK(Nuf_RecoverFor(&ops, &c, 0, 10000, old_crc, &r, &used) == NUF_OK && used == NUF_ACT_ROLLBACK_PREVIOUS, "a damaged current copy gives way to the previous one");
    }

    /* a cut of the power at every operation of a whole update: backup of the running application, staging of the new one, finish, commit over the flash. After the cut the board starts with
     * the metadata of its moment (the old image until the staging is finished, the new one after) and the start-up choice must leave one of the two images in the flash, whole. */
    {
        NufRestored r;
        uint32_t old_crc, new_crc, offset, part;
        long count = -1;
        int finished, ok;
        reset();
        fill(3, 10000);
        memcpy(old_app, flash, SLOT);
        old_crc = Norb_Crc32(old_app, 10000);
        fill(77, 12000);
        memcpy(new_app, flash, SLOT);
        new_crc = Norb_Crc32(new_app, 12000);
        for (cut = -1; cut <= count; cut++) {
            reset();
            memcpy(flash, old_app, SLOT);
            ops_left = cut;
            ok = Nuf_BackupBeforeUpdate(&ops, &c, 1, 10000, Nuf_VersionPack(0, 9, 4), &allowed) == NUF_OK;
            ok = ok && Norb_StageBegin(&ops, &c.current, 12000) == NORB_OK;
            for (offset = 0; ok && offset < 12000; offset += 4096) {
                part = 12000 - offset < 4096 ? 12000 - offset : 4096;
                ok = Norb_StagePage(&ops, &c.current, offset, new_app + offset, part) == NORB_OK;
            }
            ok = ok && Norb_StageFinish(&ops, &c.current, 12000, new_crc) == NORB_OK;
            finished = ok;
            ok = ok && Norb_Restore(&ops, &c.current, &got) == NORB_OK;
            if (cut < 0) {
                count = ops_done;
                CHECK(ok && memcmp(flash, new_app, 12000) == 0, "an update with no cut ends with the new application");
                continue;
            }
            ops_left = -1;
            {
                const uint8_t *want = finished ? new_app : old_app;
                const uint32_t want_len = finished ? 12000UL : 10000UL;
                const uint32_t want_crc = finished ? new_crc : old_crc;
                const int good = Norb_Crc32(flash, want_len) == want_crc;
                CHECK(Nuf_RecoverFor(&ops, &c, good, want_len, want_crc, &r, &used) == NUF_OK && used != NUF_ACT_LOST, "after a cut the board always starts");
                CHECK(memcmp(flash, want, want_len) == 0, "and the flash holds the image of the metadata, whole");
                CHECK(used != NUF_ACT_ROLLBACK_PREVIOUS || (r.length == 10000 && r.crc32 == old_crc && r.version == Nuf_VersionPack(0, 9, 4)), "a way back, when taken, is the old application with its version");
                CHECK(used != NUF_ACT_RESTORE_CURRENT || r.version == 0UL, "the current copy carries no version: the metadata already has it");
            }
        }
        CHECK(count > 20, "a whole update has many operations");
    }

    if (failures == 0) printf("nor_update_flow host logic tests passed\n");
    return failures == 0 ? 0 : 1;
}
