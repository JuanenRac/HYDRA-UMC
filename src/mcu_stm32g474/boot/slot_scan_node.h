/*
 * =============================================================================
 * slot_scan_node.h - how a controller board answers the slot scan probe (HAL-free, host-testable)
 * PROJECT: HYDRA-UMC
 * AUTHOR: JuanenRac (Electro Hobby 3D) - electrohobby3d@gmail.com
 * LICENSE: GPL-3.0-or-later - see repo root LICENSE
 *
 * C twin of the board side of tools/slot_scan.py. docs/SLOT_SCAN.md is the contract, tests/slot_scan_vectors.json holds the frames and the
 * delays, and tests/test_slot_scan_node.c replays them against this file. The bootloader and the application of the controller board both
 * include it (the bootloader answers with role 0, the application with role 1).
 *
 * A board never answers at once: it waits a number of steps taken from its own unique identifier, so two boards that share one slot answer
 * one after the other and the host reads two different identifiers in one window. The unique identifier of the chip is 96 bits; the board
 * folds them into 32 bits with an XOR of the three words (SlotScan_FoldUid), which is what goes on the wire.
 * =============================================================================
 */
#ifndef SLOT_SCAN_NODE_H
#define SLOT_SCAN_NODE_H

#include <stdint.h>

#define SLOT_SCAN_PROBE_MAGIC 0x5Cu
#define SLOT_SCAN_REPLY_MAGIC 0x5Du

#define SLOT_SCAN_ROLE_BOOTLOADER  0u
#define SLOT_SCAN_ROLE_APPLICATION 1u

/* docs/bus_contract.json (section slot_scan, timing_ms): proposals until the bench row 15 confirms them. */
#ifndef SLOT_SCAN_REPLY_STEP_MS
#define SLOT_SCAN_REPLY_STEP_MS 1u
#endif
#ifndef SLOT_SCAN_REPLY_STEPS
#define SLOT_SCAN_REPLY_STEPS 32u
#endif

/* The three words of the unique identifier of the chip folded into the 32 bits that the reply carries. */
static inline uint32_t SlotScan_FoldUid(uint32_t word0, uint32_t word1, uint32_t word2)
{
    return word0 ^ word1 ^ word2;
}

/* 1 and the scan id when the frame is a well-formed probe: magic, scan id, four zero bytes, complement of the magic, complement of the scan id. */
static inline uint8_t SlotScan_DecodeProbe(const uint8_t *data, uint8_t dlc, uint8_t *scan_id)
{
    if (dlc != 8u || data[0] != SLOT_SCAN_PROBE_MAGIC) return 0;
    if (data[2] != 0u || data[3] != 0u || data[4] != 0u || data[5] != 0u) return 0;
    if (data[6] != (uint8_t)(data[0] ^ 0xFFu) || data[7] != (uint8_t)(data[1] ^ 0xFFu)) return 0;
    *scan_id = data[1];
    return 1;
}

/* Reply: magic, board id (low nibble) and role (high nibble), scan id, unique identifier (32 bits, little endian), XOR of the first seven bytes. */
static inline void SlotScan_BuildReply(uint8_t out[8], uint8_t board_id, uint8_t role, uint8_t scan_id, uint32_t uid32)
{
    out[0] = SLOT_SCAN_REPLY_MAGIC;
    out[1] = (uint8_t)((board_id & 0x0Fu) | (uint8_t)(role << 4));
    out[2] = scan_id;
    out[3] = (uint8_t)(uid32 & 0xFFu);
    out[4] = (uint8_t)((uid32 >> 8) & 0xFFu);
    out[5] = (uint8_t)((uid32 >> 16) & 0xFFu);
    out[6] = (uint8_t)((uid32 >> 24) & 0xFFu);
    uint8_t check = 0;
    for (uint8_t i = 0; i < 7u; i++) check = (uint8_t)(check ^ out[i]);
    out[7] = check;
}

/* How long a board waits before it answers: a step per value of its unique identifier modulo the number of steps. The multiplier spreads the low
 * bits of the identifier, which a series of boards often shares. */
static inline uint32_t SlotScan_ReplyDelayMs(uint32_t uid32)
{
    const uint32_t spread = (uint32_t)(uid32 * 2654435761u);
    return (spread % SLOT_SCAN_REPLY_STEPS) * SLOT_SCAN_REPLY_STEP_MS;
}

/* One answer waiting for its time. A new probe replaces an older one that has not been sent yet. */
typedef struct {
    uint8_t pending;
    uint8_t scan_id;
    uint32_t due_ms;
} SlotScanNode_t;

static inline void SlotScanNode_Reset(SlotScanNode_t *n)
{
    n->pending = 0;
    n->scan_id = 0;
    n->due_ms = 0;
}

/* Feeds a frame received at the probe offset of this board's own slot window. Returns 1 when it was a probe and an answer is now scheduled. */
static inline uint8_t SlotScanNode_OnProbe(SlotScanNode_t *n, uint32_t now_ms, const uint8_t *data, uint8_t dlc, uint32_t uid32)
{
    uint8_t scan_id;
    if (!SlotScan_DecodeProbe(data, dlc, &scan_id)) return 0;
    n->pending = 1;
    n->scan_id = scan_id;
    n->due_ms = (uint32_t)(now_ms + SlotScan_ReplyDelayMs(uid32));
    return 1;
}

/* Call every pass: returns 1 and the 8 data bytes of the reply once its time has come (and clears it), 0 otherwise. */
static inline uint8_t SlotScanNode_Due(SlotScanNode_t *n, uint32_t now_ms, uint8_t board_id, uint8_t role, uint32_t uid32, uint8_t out[8])
{
    if (!n->pending || (int32_t)((uint32_t)(now_ms - n->due_ms)) < 0) return 0;
    n->pending = 0;
    SlotScan_BuildReply(out, board_id, role, n->scan_id, uid32);
    return 1;
}

#endif /* SLOT_SCAN_NODE_H */
