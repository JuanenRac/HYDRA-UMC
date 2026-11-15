/* =============================================================================
 * HYDRA-UMC Firmware - src/mcu_stm32h757/Common/nor_backup_plan.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * nor_backup_plan.h - the backup copy of the application of the CM7 when its slot is 768 KB (HAL-free, host-testable). With the slot extended to the end of bank 1 there is no room
 * left in the internal flash for a second copy, so the copy lives in the NOR flash of SPI5 (16 MB) and the bootloader restores it when the application in the flash is not good.
 *
 * Layout in the NOR, from `nor_offset` (a multiple of the 64 KB block): two header sectors of 4 KB (A and B, written alternately so that a cut of the power during a save always leaves
 * one good header), the rest of the first block unused, then the image from the next block on. A save runs while the application of the flash is good: it writes the image first and the header last, so a cut of the power at any point of a save leaves the application untouched and a
 * copy that the check of the image refuses (the image in the NOR no longer matches the header that was there), never a half-written copy that is trusted; the next save makes it good again. A restore reads the newer valid header,
 * checks the CRC-32 of the image IN THE NOR before touching the flash, then erases, writes and checks the CRC-32 of what was written; a failed check is reported, never hidden.
 *
 * Everything reaches the memories through the callbacks of NorbOps, so the same code runs on the host against two RAM doubles and on the board against the SPI5 driver and the flash
 * driver. The sector and block sizes are those of the GD25Q128E. tools/nor_backup_plan.py is the Python twin and reads config/nor_backup.cfg. The use of the copy ships off
 * (HYDRA_NOR_BACKUP 0): nothing in the bootloader calls this file yet. Evidence: simulator; nothing here has run on a board.
 * ============================================================================= */
#ifndef HYDRA_UMC_NOR_BACKUP_PLAN_H
#define HYDRA_UMC_NOR_BACKUP_PLAN_H

#include <stdint.h>

/* starting values (config/nor_backup.cfg): the copy at the start of the NOR, the 768 KB slot of the application of the CM7 */
#define NORB_DEFAULT_NOR_OFFSET 0UL
#define NORB_DEFAULT_NOR_SIZE 16777216UL
#define NORB_DEFAULT_SLOT_BYTES 786432UL

#define NORB_SECTOR 4096UL
#define NORB_BLOCK 65536UL
#define NORB_HEADER_BYTES 24UL
#define NORB_MAGIC 0x42524F4EUL /* "NORB" */
#define NORB_CHUNK 256UL

#define NORB_OK 0
#define NORB_E_ARG 1
#define NORB_E_ALIGN 2 /* the offset or the slot is not a multiple of the erase unit */
#define NORB_E_RANGE 3 /* the copy does not fit in the NOR */
#define NORB_E_TOO_BIG 4
#define NORB_E_NOR_READ 5
#define NORB_E_NOR_WRITE 6
#define NORB_E_FLASH_WRITE 7
#define NORB_E_FLASH_READ 8
#define NORB_E_NO_COPY 9 /* neither header is good */
#define NORB_E_COPY_BAD 10 /* the image in the NOR does not match its header */
#define NORB_E_VERIFY 11 /* what was written to the flash does not read back as the copy */

#define NORB_ACT_NONE 0    /* the application is good: nothing to do */
#define NORB_ACT_RESTORE 1 /* the application is not good and a copy exists */
#define NORB_ACT_LOST 2    /* the application is not good and there is no copy */

typedef struct {
    uint32_t nor_offset, nor_size, slot_bytes;
} NorbConfig;

typedef struct {
    uint32_t header_a, header_b; /* offsets in the NOR */
    uint32_t image_offset, image_bytes_max;
    uint32_t end; /* the first byte after the copy */
} NorbLayout;

typedef struct {
    uint32_t sequence, length, crc32;
} NorbHeader;

typedef struct {
    int (*nor_read)(void *ctx, uint32_t offset, uint8_t *data, uint32_t length);
    int (*nor_erase)(void *ctx, uint32_t offset, uint32_t length);
    int (*nor_write)(void *ctx, uint32_t offset, const uint8_t *data, uint32_t length);
    int (*flash_read)(void *ctx, uint32_t offset, uint8_t *data, uint32_t length); /* offset from the start of the application slot */
    int (*flash_erase)(void *ctx, uint32_t length);                                /* erases the application slot (length rounded up by the driver) */
    int (*flash_write)(void *ctx, uint32_t offset, const uint8_t *data, uint32_t length);
    void *ctx;
} NorbOps;

static inline void Norb_Defaults(NorbConfig *c)
{
    c->nor_offset = NORB_DEFAULT_NOR_OFFSET;
    c->nor_size = NORB_DEFAULT_NOR_SIZE;
    c->slot_bytes = NORB_DEFAULT_SLOT_BYTES;
}

static inline uint32_t Norb_Crc32Update(uint32_t crc, const uint8_t *data, uint32_t length)
{
    uint32_t i, bit;
    for (i = 0; i < length; i++) {
        crc ^= data[i];
        for (bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xEDB88320UL & (0UL - (crc & 1UL)));
    }
    return crc;
}

/* the CRC-32 of a block of bytes (the same as zlib.crc32) */
static inline uint32_t Norb_Crc32(const uint8_t *data, uint32_t length)
{
    return ~Norb_Crc32Update(0xFFFFFFFFUL, data, length);
}

/* where everything goes; refuses an offset or a slot that is not a multiple of the erase unit and a copy that does not fit in the NOR */
static inline int Norb_Plan(const NorbConfig *c, NorbLayout *l)
{
    uint32_t image_bytes;
    if (c == 0 || l == 0) return NORB_E_ARG;
    if (c->slot_bytes == 0 || c->nor_size == 0) return NORB_E_ARG;
    if (c->nor_offset % NORB_BLOCK != 0 || c->slot_bytes % NORB_SECTOR != 0) return NORB_E_ALIGN;
    image_bytes = ((c->slot_bytes + NORB_BLOCK - 1UL) / NORB_BLOCK) * NORB_BLOCK;
    if (c->nor_offset > c->nor_size || c->nor_size - c->nor_offset < NORB_BLOCK + image_bytes) return NORB_E_RANGE;
    l->header_a = c->nor_offset;
    l->header_b = c->nor_offset + NORB_SECTOR;
    l->image_offset = c->nor_offset + NORB_BLOCK;
    l->image_bytes_max = c->slot_bytes;
    l->end = l->image_offset + image_bytes;
    return NORB_OK;
}

static inline void Norb_Put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static inline uint32_t Norb_Get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* the 24 bytes of a header: magic, sequence, length, CRC-32 of the image, a zero word, CRC-32 of the first 20 bytes (all little-endian) */
static inline void Norb_HeaderEncode(const NorbHeader *h, uint8_t out[NORB_HEADER_BYTES])
{
    Norb_Put32(out, NORB_MAGIC);
    Norb_Put32(out + 4, h->sequence);
    Norb_Put32(out + 8, h->length);
    Norb_Put32(out + 12, h->crc32);
    Norb_Put32(out + 16, 0UL);
    Norb_Put32(out + 20, Norb_Crc32(out, 20UL));
}

/* 1 when the bytes are a header (right magic, right CRC, a length that is neither 0 nor more than the slot) */
static inline int Norb_HeaderDecode(const uint8_t in[NORB_HEADER_BYTES], uint32_t slot_bytes, NorbHeader *h)
{
    if (Norb_Get32(in) != NORB_MAGIC) return 0;
    if (Norb_Get32(in + 20) != Norb_Crc32(in, 20UL)) return 0;
    if (Norb_Get32(in + 8) == 0UL || Norb_Get32(in + 8) > slot_bytes) return 0;
    h->sequence = Norb_Get32(in + 4);
    h->length = Norb_Get32(in + 8);
    h->crc32 = Norb_Get32(in + 12);
    return 1;
}

/* the newer of the two headers (a sequence that is ahead by wrapping arithmetic wins); 0 when neither is good. `which` says 0 for A and 1 for B. */
static inline int Norb_ReadHeaders(const NorbOps *ops, const NorbLayout *l, uint32_t slot_bytes, NorbHeader *newest, int *which)
{
    uint8_t raw[NORB_HEADER_BYTES];
    NorbHeader a, b;
    int ok_a = 0, ok_b = 0;
    if (ops->nor_read(ops->ctx, l->header_a, raw, NORB_HEADER_BYTES) == 0) ok_a = Norb_HeaderDecode(raw, slot_bytes, &a);
    if (ops->nor_read(ops->ctx, l->header_b, raw, NORB_HEADER_BYTES) == 0) ok_b = Norb_HeaderDecode(raw, slot_bytes, &b);
    if (ok_a && ok_b) {
        if ((int32_t)(b.sequence - a.sequence) > 0) {
            *newest = b;
            *which = 1;
        } else {
            *newest = a;
            *which = 0;
        }
        return 1;
    }
    if (ok_a) {
        *newest = a;
        *which = 0;
        return 1;
    }
    if (ok_b) {
        *newest = b;
        *which = 1;
        return 1;
    }
    return 0;
}

/* what the bootloader does when it has judged the application of the flash: nothing, restore the copy, or report that it is lost */
static inline int Norb_Decide(int application_good, int copy_exists)
{
    if (application_good) return NORB_ACT_NONE;
    return copy_exists ? NORB_ACT_RESTORE : NORB_ACT_LOST;
}

/* the CRC-32 of `length` bytes of the NOR, read in chunks */
static inline int Norb_NorCrc(const NorbOps *ops, uint32_t offset, uint32_t length, uint32_t *crc_out)
{
    uint8_t buf[NORB_CHUNK];
    uint32_t done = 0, n, crc = 0xFFFFFFFFUL;
    while (done < length) {
        n = (length - done < NORB_CHUNK) ? length - done : NORB_CHUNK;
        if (ops->nor_read(ops->ctx, offset + done, buf, n) != 0) return NORB_E_NOR_READ;
        crc = Norb_Crc32Update(crc, buf, n);
        done += n;
    }
    *crc_out = ~crc;
    return NORB_OK;
}

/* Writes a header into the sector that is not the newest (`into_b` says B): erase, then program. */
static inline int Norb_WriteHeader(const NorbOps *ops, const NorbLayout *l, const NorbHeader *h, int into_b)
{
    uint8_t raw[NORB_HEADER_BYTES];
    const uint32_t where = into_b ? l->header_b : l->header_a;
    Norb_HeaderEncode(h, raw);
    if (ops->nor_erase(ops->ctx, where, NORB_SECTOR) != 0) return NORB_E_NOR_WRITE;
    if (ops->nor_write(ops->ctx, where, raw, NORB_HEADER_BYTES) != 0) return NORB_E_NOR_WRITE;
    return NORB_OK;
}

/* Saves the application of the flash (`length` bytes) into the NOR: image first, header last, into the header that is NOT the newest. */
static inline int Norb_Save(const NorbOps *ops, const NorbConfig *c, uint32_t length)
{
    NorbLayout l;
    NorbHeader newest, h;
    uint8_t buf[NORB_CHUNK];
    uint32_t done = 0, n, crc = 0xFFFFFFFFUL;
    int which = 1, have, rc;
    rc = Norb_Plan(c, &l);
    if (rc != NORB_OK) return rc;
    if (length == 0UL || length > c->slot_bytes) return NORB_E_TOO_BIG;
    have = Norb_ReadHeaders(ops, &l, c->slot_bytes, &newest, &which);
    h.sequence = have ? newest.sequence + 1UL : 1UL;
    if (ops->nor_erase(ops->ctx, l.image_offset, ((length + NORB_SECTOR - 1UL) / NORB_SECTOR) * NORB_SECTOR) != 0) return NORB_E_NOR_WRITE;
    while (done < length) {
        n = (length - done < NORB_CHUNK) ? length - done : NORB_CHUNK;
        if (ops->flash_read(ops->ctx, done, buf, n) != 0) return NORB_E_FLASH_READ;
        if (ops->nor_write(ops->ctx, l.image_offset + done, buf, n) != 0) return NORB_E_NOR_WRITE;
        crc = Norb_Crc32Update(crc, buf, n);
        done += n;
    }
    h.length = length;
    h.crc32 = ~crc;
    return Norb_WriteHeader(ops, &l, &h, have && which == 0);
}

/* A staged update (the 768 KB slot has no room in the flash for the second copy, so the update that arrives page by page is written into the image area of the NOR, checked there and
 * then copied into the slot by Norb_Restore). Begin erases the image area; a header that described an older copy stops matching, so nothing half-staged is ever restored. */
static inline int Norb_StageBegin(const NorbOps *ops, const NorbConfig *c, uint32_t total)
{
    NorbLayout l;
    int rc = Norb_Plan(c, &l);
    if (rc != NORB_OK) return rc;
    if (total == 0UL || total > c->slot_bytes) return NORB_E_TOO_BIG;
    if (ops->nor_erase(ops->ctx, l.image_offset, ((total + NORB_SECTOR - 1UL) / NORB_SECTOR) * NORB_SECTOR) != 0) return NORB_E_NOR_WRITE;
    return NORB_OK;
}

/* One page of the update at `offset` of the image (a page may not run past the slot). */
static inline int Norb_StagePage(const NorbOps *ops, const NorbConfig *c, uint32_t offset, const uint8_t *data, uint32_t length)
{
    NorbLayout l;
    int rc = Norb_Plan(c, &l);
    if (rc != NORB_OK) return rc;
    if (length == 0UL || offset > c->slot_bytes || length > c->slot_bytes - offset) return NORB_E_TOO_BIG;
    return ops->nor_write(ops->ctx, l.image_offset + offset, data, length) == 0 ? NORB_OK : NORB_E_NOR_WRITE;
}

/* The update is complete: the CRC-32 of what is in the NOR must be the one the sender gave, and only then the header is written (last, into the older sector). */
static inline int Norb_StageFinish(const NorbOps *ops, const NorbConfig *c, uint32_t total, uint32_t crc32)
{
    NorbLayout l;
    NorbHeader newest, h;
    uint32_t crc = 0;
    int which = 1, have, rc;
    rc = Norb_Plan(c, &l);
    if (rc != NORB_OK) return rc;
    if (total == 0UL || total > c->slot_bytes) return NORB_E_TOO_BIG;
    rc = Norb_NorCrc(ops, l.image_offset, total, &crc);
    if (rc != NORB_OK) return rc;
    if (crc != crc32) return NORB_E_COPY_BAD;
    have = Norb_ReadHeaders(ops, &l, c->slot_bytes, &newest, &which);
    h.sequence = have ? newest.sequence + 1UL : 1UL;
    h.length = total;
    h.crc32 = crc32;
    return Norb_WriteHeader(ops, &l, &h, have && which == 0);
}

/* 1 when a copy that can be restored exists (a good header whose image in the NOR matches); 0 otherwise */
static inline int Norb_CopyExists(const NorbOps *ops, const NorbConfig *c)
{
    NorbLayout l;
    NorbHeader h;
    uint32_t crc;
    int which;
    if (Norb_Plan(c, &l) != NORB_OK) return 0;
    if (!Norb_ReadHeaders(ops, &l, c->slot_bytes, &h, &which)) return 0;
    if (Norb_NorCrc(ops, l.image_offset, h.length, &crc) != NORB_OK) return 0;
    return crc == h.crc32;
}

/* Restores the copy into the application slot of the flash. The image in the NOR is checked BEFORE the flash is erased, and what is written is read back and checked. */
static inline int Norb_Restore(const NorbOps *ops, const NorbConfig *c, uint32_t *restored_length)
{
    NorbLayout l;
    NorbHeader h;
    uint8_t buf[NORB_CHUNK];
    uint32_t done = 0, n, crc;
    int which, rc;
    rc = Norb_Plan(c, &l);
    if (rc != NORB_OK) return rc;
    if (!Norb_ReadHeaders(ops, &l, c->slot_bytes, &h, &which)) return NORB_E_NO_COPY;
    rc = Norb_NorCrc(ops, l.image_offset, h.length, &crc);
    if (rc != NORB_OK) return rc;
    if (crc != h.crc32) return NORB_E_COPY_BAD;
    if (ops->flash_erase(ops->ctx, h.length) != 0) return NORB_E_FLASH_WRITE;
    while (done < h.length) {
        n = (h.length - done < NORB_CHUNK) ? h.length - done : NORB_CHUNK;
        if (ops->nor_read(ops->ctx, l.image_offset + done, buf, n) != 0) return NORB_E_NOR_READ;
        if (ops->flash_write(ops->ctx, done, buf, n) != 0) return NORB_E_FLASH_WRITE;
        done += n;
    }
    crc = 0xFFFFFFFFUL;
    for (done = 0; done < h.length; done += n) {
        n = (h.length - done < NORB_CHUNK) ? h.length - done : NORB_CHUNK;
        if (ops->flash_read(ops->ctx, done, buf, n) != 0) return NORB_E_FLASH_READ;
        crc = Norb_Crc32Update(crc, buf, n);
    }
    if (~crc != h.crc32) return NORB_E_VERIFY;
    if (restored_length != 0) *restored_length = h.length;
    return NORB_OK;
}

#endif /* HYDRA_UMC_NOR_BACKUP_PLAN_H */
