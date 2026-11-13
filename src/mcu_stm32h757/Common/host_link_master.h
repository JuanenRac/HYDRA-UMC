/* =============================================================================
 * HYDRA-UMC Firmware - src/mcu_stm32h757/Common/host_link_master.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * host_link_master.h - the transport of the host link with the board as SPI master (HAL-free, host-testable). The C twin of tools/host_link_master.py (read its header and
 * docs/HOST_LINK_MASTER.md): the sealed 128-byte frame of the slave link is kept and only who clocks changes. One call of HostLink_Poll runs at most one transaction (one frame out,
 * one frame in) when a frame is staged, the READY line of the host is high, or the idle interval has passed; two transactions are never closer than the minimum gap.
 * A frame in that is damaged or (with the seal required) unsealed is not dispatched, and the frame that went out is sent again with the same sequence a bounded number of times;
 * a repeated sequence is not run twice; a frame of 0xFF in every byte is the line floating (silence). The peripheral is reached only through HostLinkOps. The link never carries the
 * emergency stop. tests/test_host_link_master.c runs it against the vectors written from the Python reference. Nothing here has run on a board.
 * ============================================================================= */
#ifndef HYDRA_UMC_HOST_LINK_MASTER_H
#define HYDRA_UMC_HOST_LINK_MASTER_H

#include <stdint.h>
#include <string.h>

#include "spi_frame_crc.h"

#define HOST_LINK_FRAME_BYTES 128u
#define HOST_LINK_HEAD_BYTES 12u
#define HOST_LINK_IDLE_TYPE 0xFFu

typedef enum { HOST_LINK_POLL_NOT_DUE = 0, HOST_LINK_POLL_NOTHING = 1, HOST_LINK_POLL_DONE = 2, HOST_LINK_POLL_XFER_FAIL = 3 } HostLinkPoll;
typedef enum {
    HOST_LINK_RX_NONE = 0,
    HOST_LINK_RX_DISPATCH = 1,
    HOST_LINK_RX_DUPLICATE = 2,
    HOST_LINK_RX_BAD_CRC = 3,
    HOST_LINK_RX_UNSEALED = 4,
    HOST_LINK_RX_IDLE = 5,
    HOST_LINK_RX_SILENCE = 6
} HostLinkRx;

typedef struct {
    uint32_t idle_interval_ms;
    uint32_t min_gap_ms;
    uint8_t max_retries;
    uint8_t down_after_failures;
    uint8_t require_seal;
    uint8_t ready_known;
} HostLinkConfig;

typedef struct {
    /* one transaction: send 128 bytes and read the 128 that come in; 1 when the peripheral worked, 0 when it failed */
    int (*transfer)(void *ctx, const uint8_t *tx, uint8_t *rx);
    int (*ready)(void *ctx); /* the READY line of the host: 1 when high; not called when the line is not wired (ready_known 0) */
    /* runs a request that was not a duplicate; writes the 12 bytes of the frame that answers and returns 1, or returns 0 when there is no answer */
    int (*dispatch)(void *ctx, const uint8_t *request, uint8_t *answer12);
} HostLinkOps;

typedef struct {
    uint32_t transactions, dispatched, duplicates, bad_crc, unsealed, idle_rx, silence, transfer_failures, retries, dropped;
} HostLinkCounters;

typedef struct {
    HostLinkConfig cfg;
    HostLinkOps ops;
    void *ctx;
    HostLinkCounters counters;
    uint8_t staged_valid;
    uint8_t staged[HOST_LINK_HEAD_BYTES];
    uint8_t staged_seq;
    uint8_t attempts;
    uint8_t tx_seq;
    uint8_t have_last;
    uint8_t last_rx_seq;
    uint32_t last_time;
    uint8_t ran_once;
    uint8_t failures_in_a_row;
    uint8_t down;
    uint8_t last_rx_verdict;
    uint8_t last_tx_type;
    uint8_t last_tx_seq;
} HostLink_t;

/* Returns 0 when the configuration cannot work (the same limits as config/host_link.cfg) or an operation is missing. */
static inline int HostLink_Init(HostLink_t *l, const HostLinkConfig *cfg, const HostLinkOps *ops, void *ctx)
{
    if (cfg->idle_interval_ms < 1u || cfg->idle_interval_ms > 10000u || cfg->min_gap_ms > 1000u || cfg->min_gap_ms > cfg->idle_interval_ms) return 0;
    if (cfg->max_retries > 10u || cfg->down_after_failures < 1u || cfg->down_after_failures > 100u) return 0;
    if (cfg->require_seal > 1u || cfg->ready_known > 1u) return 0;
    if (!ops->transfer || !ops->dispatch || (cfg->ready_known && !ops->ready)) return 0;
    memset(l, 0, sizeof *l);
    l->cfg = *cfg;
    l->ops = *ops;
    l->ctx = ctx;
    return 1;
}

/* Puts one frame on the line (its first 12 bytes; the seal is written at the transaction). 0 when the slot is taken. */
static inline int HostLink_Stage(HostLink_t *l, const uint8_t *frame12)
{
    if (l->staged_valid) return 0;
    memcpy(l->staged, frame12, HOST_LINK_HEAD_BYTES);
    l->staged_valid = 1u;
    l->tx_seq = (uint8_t)(l->tx_seq + 1u);
    l->staged_seq = l->tx_seq;
    l->attempts = 0;
    return 1;
}

static inline int HostLink_IsSilence(const uint8_t *wire)
{
    for (uint32_t i = 0; i < HOST_LINK_FRAME_BYTES; i++) {
        if (wire[i] != 0xFFu) return 0;
    }
    return 1;
}

static inline uint8_t HostLink_Take(HostLink_t *l, const uint8_t *wire)
{
    uint8_t verdict;
    uint8_t damaged = 0;
    uint8_t sequence = 0;
    if (HostLink_IsSilence(wire)) {
        l->counters.silence++;
        verdict = HOST_LINK_RX_SILENCE;
    } else {
        const SpiWireState state = SpiWire_Check(wire, &sequence);
        if (state == SPI_WIRE_SEALED_BAD) {
            l->counters.bad_crc++;
            verdict = HOST_LINK_RX_BAD_CRC;
            damaged = 1;
        } else if (state == SPI_WIRE_UNSEALED && l->cfg.require_seal) {
            l->counters.unsealed++;
            verdict = HOST_LINK_RX_UNSEALED;
            damaged = 1;
        } else if (l->have_last && state == SPI_WIRE_SEALED_OK && sequence == l->last_rx_seq) {
            l->counters.duplicates++;
            verdict = HOST_LINK_RX_DUPLICATE;
        } else {
            if (state == SPI_WIRE_SEALED_OK) {
                l->have_last = 1u;
                l->last_rx_seq = sequence;
            }
            if (wire[2] == HOST_LINK_IDLE_TYPE) {
                l->counters.idle_rx++;
                verdict = HOST_LINK_RX_IDLE;
            } else {
                l->counters.dispatched++;
                verdict = HOST_LINK_RX_DISPATCH;
            }
        }
    }
    if (damaged && l->staged_valid) {
        l->attempts++;
        if (l->attempts > l->cfg.max_retries) {
            l->staged_valid = 0;
            l->counters.dropped++;
        } else {
            l->counters.retries++;
        }
    } else if (l->staged_valid && verdict != HOST_LINK_RX_SILENCE) {
        l->staged_valid = 0; /* a good frame came back in the transaction that carried ours; with silence nobody took it, so it stays */
    }
    if (verdict == HOST_LINK_RX_DISPATCH) {
        uint8_t answer[HOST_LINK_HEAD_BYTES];
        if (l->ops.dispatch(l->ctx, wire, answer)) (void)HostLink_Stage(l, answer);
    }
    return verdict;
}

static inline HostLinkPoll HostLink_Poll(HostLink_t *l, uint32_t now_ms)
{
    const uint32_t since = (uint32_t)(now_ms - l->last_time);
    if (l->ran_once && since < l->cfg.min_gap_ms) return HOST_LINK_POLL_NOT_DUE;
    const uint8_t idle_over = (uint8_t)(!l->ran_once || since >= l->cfg.idle_interval_ms);
    const uint8_t staged_wants = (uint8_t)(l->staged_valid && l->last_rx_verdict != HOST_LINK_RX_SILENCE);
    /* a staged frame does not make the board clock a line that answered with silence at the full rate: it waits for the idle interval or for READY */
    if (!(staged_wants || (l->cfg.ready_known && l->ops.ready(l->ctx)) || idle_over)) return HOST_LINK_POLL_NOTHING;

    uint8_t tx[HOST_LINK_FRAME_BYTES];
    uint8_t seq;
    memset(tx, 0xFF, sizeof tx);
    if (l->staged_valid) {
        seq = l->staged_seq;
        memcpy(tx, l->staged, HOST_LINK_HEAD_BYTES);
        /* the length byte of the frame says how many of the 8 payload bytes are real; the others go out as zero, as the Python reference builds the frame */
        const uint8_t length = tx[3] > 8u ? 8u : tx[3];
        memset(tx + 4, 0, 8);
        memcpy(tx + 4, l->staged + 4, length);
        tx[3] = length;
    } else {
        l->tx_seq = (uint8_t)(l->tx_seq + 1u);
        seq = l->tx_seq;
        tx[0] = 0;
        tx[1] = 0;
        tx[2] = HOST_LINK_IDLE_TYPE;
        tx[3] = 0;
        memset(tx + 4, 0, 8);
    }
    SpiWire_Seal(tx, seq);
    l->last_time = now_ms;
    l->ran_once = 1u;
    l->last_tx_type = tx[2];
    l->last_tx_seq = seq;

    uint8_t rx[HOST_LINK_FRAME_BYTES];
    if (!l->ops.transfer(l->ctx, tx, rx)) {
        l->counters.transfer_failures++;
        if (l->failures_in_a_row < 255u) l->failures_in_a_row++;
        if (l->failures_in_a_row >= l->cfg.down_after_failures) l->down = 1u;
        l->last_rx_verdict = HOST_LINK_RX_NONE;
        return HOST_LINK_POLL_XFER_FAIL;
    }
    l->failures_in_a_row = 0;
    l->down = 0;
    l->counters.transactions++;
    l->last_rx_verdict = HostLink_Take(l, rx);
    return HOST_LINK_POLL_DONE;
}

#endif /* HYDRA_UMC_HOST_LINK_MASTER_H */
