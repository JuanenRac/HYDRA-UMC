/*
 * =============================================================================
 * slot_scan_master.h - the whole slot scan on the master side: who is in a slot, and whether two boards share it (HAL-free, host-testable)
 * PROJECT: HYDRA-UMC
 * AUTHOR: JuanenRac (Electro Hobby 3D) - electrohobby3d@gmail.com
 * LICENSE: GPL-3.0-or-later - see repo root LICENSE
 *
 * C twin of Heard/classify() of tools/slot_scan.py (the verdict) and of encode_probe()/decode_reply(). docs/SLOT_SCAN.md is the contract and
 * tests/slot_scan_vectors.json holds the frames and the scenarios; tests/test_slot_scan_master.c replays all of them against this file.
 *
 * The scan of the application distinguishes only present and absent (it asks AXIS_STATUS). This file gives the full verdict of one slot:
 *   ABSENT    nothing was heard
 *   OK        one board (one unique identifier, however often it repeated) whose board id is the slot
 *   DUPLICATE two unique identifiers, or error frames next to a readable answer (two boards that collided)
 *   MISPLACED one board whose board id is not the slot of the window
 *   NOISY     error frames and no readable answer
 * and an update may write to a slot only on OK (SlotScanMaster_WriteAllowed). Answers of another scan id and frames that do not decode are
 * ignored, so a late or damaged frame never counts. Whatever cannot be told is never OK: a scan that was not begun gives ABSENT and allows nothing.
 *
 * Nothing here sends or receives: the caller begins the scan of a slot, hands in each frame heard in the window and each bus error, and reads
 * the verdict when the window ends. Nothing in this file has run on a board.
 * =============================================================================
 */
#ifndef SLOT_SCAN_MASTER_H
#define SLOT_SCAN_MASTER_H

#include <stdint.h>

#ifndef SLOT_SCAN_PROBE_MAGIC
#define SLOT_SCAN_PROBE_MAGIC 0x5Cu
#endif
#ifndef SLOT_SCAN_REPLY_MAGIC
#define SLOT_SCAN_REPLY_MAGIC 0x5Du
#endif
#define SLOT_SCAN_MASTER_SLOTS 8u

typedef enum {
    SLOT_SCAN_ABSENT = 0,
    SLOT_SCAN_OK = 1,
    SLOT_SCAN_DUPLICATE = 2,
    SLOT_SCAN_MISPLACED = 3,
    SLOT_SCAN_NOISY = 4
} SlotScanVerdict;

typedef struct {
    uint8_t active;      /* a scan of a slot was begun */
    uint8_t slot;        /* the slot of the window */
    uint8_t scan_id;     /* the scan this window answers */
    uint8_t replies;     /* readable answers of this scan heard in the window (counted up to 255) */
    uint8_t first_board; /* board id of the first readable answer */
    uint8_t distinct;    /* different unique identifiers heard: 0, 1 or 2 (two is as many as the verdict needs) */
    uint32_t first_uid;
    uint16_t bus_errors; /* error frames heard in the window (saturates) */
} SlotScanMaster;

/* The probe of a scan: magic, scan id, four zero bytes, the complements of the magic and of the scan id. */
static inline void SlotScanMaster_EncodeProbe(uint8_t scan_id, uint8_t out[8])
{
    out[0] = SLOT_SCAN_PROBE_MAGIC;
    out[1] = scan_id;
    out[2] = out[3] = out[4] = out[5] = 0u;
    out[6] = (uint8_t)(SLOT_SCAN_PROBE_MAGIC ^ 0xFFu);
    out[7] = (uint8_t)(scan_id ^ 0xFFu);
}

/* Starts the window of one slot. A slot outside 0..7 does not start a scan (the verdict then stays ABSENT and writes nothing). */
static inline void SlotScanMaster_Begin(SlotScanMaster *m, uint8_t slot, uint8_t scan_id)
{
    m->active = 0u;
    m->slot = slot;
    m->scan_id = scan_id;
    m->replies = 0u;
    m->first_board = 0u;
    m->distinct = 0u;
    m->first_uid = 0u;
    m->bus_errors = 0u;
    if (slot < SLOT_SCAN_MASTER_SLOTS) m->active = 1u;
}

/* 1 when the frame is a well-formed reply (length, magic, XOR of the first seven bytes, board id below 8, role 0 or 1); the fields are filled in. */
static inline uint8_t SlotScanMaster_DecodeReply(const uint8_t *data, uint8_t dlc, uint8_t *board_id, uint8_t *role, uint8_t *scan_id, uint32_t *uid32)
{
    if (dlc != 8u || data[0] != SLOT_SCAN_REPLY_MAGIC) return 0;
    uint8_t check = 0u;
    for (uint8_t i = 0u; i < 7u; i++) check ^= data[i];
    if (check != data[7]) return 0;
    *board_id = (uint8_t)(data[1] & 0x0Fu);
    *role = (uint8_t)(data[1] >> 4);
    if (*board_id >= SLOT_SCAN_MASTER_SLOTS || *role > 1u) return 0;
    *scan_id = data[2];
    *uid32 = (uint32_t)data[3] | ((uint32_t)data[4] << 8) | ((uint32_t)data[5] << 16) | ((uint32_t)data[6] << 24);
    return 1;
}

/* A frame heard in the window. Returns 1 when it counted as an answer of this scan, 0 when it was ignored (no scan, damaged, another scan). */
static inline uint8_t SlotScanMaster_OnFrame(SlotScanMaster *m, const uint8_t *data, uint8_t dlc)
{
    uint8_t board, role, scan;
    uint32_t uid;
    if (!m->active || !SlotScanMaster_DecodeReply(data, dlc, &board, &role, &scan, &uid) || scan != m->scan_id) return 0;
    if (m->replies == 0u) {
        m->first_board = board;
        m->first_uid = uid;
        m->distinct = 1u;
    } else if (m->distinct == 1u && uid != m->first_uid) {
        m->distinct = 2u;
    }
    if (m->replies < 255u) m->replies++;
    return 1;
}

/* An error frame heard in the window. */
static inline void SlotScanMaster_OnBusError(SlotScanMaster *m)
{
    if (m->active && m->bus_errors < 0xFFFFu) m->bus_errors++;
}

/* The verdict of the window; read it when the window ends. */
static inline SlotScanVerdict SlotScanMaster_Verdict(const SlotScanMaster *m)
{
    if (!m->active) return SLOT_SCAN_ABSENT;
    if (m->distinct > 1u) return SLOT_SCAN_DUPLICATE;
    if (m->replies == 0u) return m->bus_errors ? SLOT_SCAN_NOISY : SLOT_SCAN_ABSENT;
    if (m->first_board != m->slot) return SLOT_SCAN_MISPLACED;
    /* error frames next to a single readable answer: another board of the slot collided with it */
    return m->bus_errors ? SLOT_SCAN_DUPLICATE : SLOT_SCAN_OK;
}

/* An update may write to a slot only when exactly one board is there and the slot is the one it says. */
static inline uint8_t SlotScanMaster_WriteAllowed(SlotScanVerdict verdict)
{
    return verdict == SLOT_SCAN_OK ? 1u : 0u;
}

#endif /* SLOT_SCAN_MASTER_H */
