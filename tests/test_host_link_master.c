/* =============================================================================
 * HYDRA-UMC Firmware - tests/test_host_link_master.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Host test of the transport of the host link with the board as SPI master (src/mcu_stm32h757/Common/host_link_master.h) against the vectors written from the Python reference
 * (tests/host_link_vectors.h): the same poll result, verdict, frame out, counters and state after every call, plus the refusal of configurations that cannot work. Evidence: simulator.
 *
 *   cc -std=c11 -Wall -Wextra -Itests -Isrc/mcu_stm32h757/Common -Isrc/mcu_stm32h757/CM4/boot -o test_host_link_master tests/test_host_link_master.c
 */
#include <stdio.h>
#include <string.h>

#include "host_link_master.h"
#include "host_link_vectors.h"

#define KIND_SILENCE 0
#define KIND_IDLE 1
#define KIND_REQUEST 2
#define KIND_DAMAGED 3
#define KIND_UNSEALED 4

typedef struct {
    const hl_in_t *step;
    uint8_t sent[HOST_LINK_FRAME_BYTES];
    int sent_sealed_ok;
    int dispatch_calls;
} Double;

static void host_frame(const hl_in_t *s, uint8_t *w)
{
    memset(w, 0xFF, HOST_LINK_FRAME_BYTES);
    if (s->kind == KIND_SILENCE) return;
    w[0] = 0;
    w[1] = 0;
    memset(w + 4, 0, 8);
    if (s->kind == KIND_IDLE) {
        w[2] = HOST_LINK_IDLE_TYPE;
        w[3] = 0;
        SpiWire_Seal(w, s->rseq);
        return;
    }
    w[2] = s->rtype;
    w[3] = 1;
    w[4] = s->rbyte;
    if (s->kind == KIND_UNSEALED) return;
    SpiWire_Seal(w, s->rseq);
    if (s->kind == KIND_DAMAGED) w[SPI_WIRE_OFS_CRC + 1u] ^= 0xFFu;
}

static int d_transfer(void *ctx, const uint8_t *tx, uint8_t *rx)
{
    Double *d = (Double *)ctx;
    uint8_t seq;
    memcpy(d->sent, tx, HOST_LINK_FRAME_BYTES);
    d->sent_sealed_ok = SpiWire_Check(tx, &seq) == SPI_WIRE_SEALED_OK;
    if (!d->step->xfer_ok) return 0;
    host_frame(d->step, rx);
    return 1;
}

static int d_ready(void *ctx) { return ((Double *)ctx)->step->ready; }

static int d_dispatch(void *ctx, const uint8_t *request, uint8_t *answer)
{
    ((Double *)ctx)->dispatch_calls++;
    if (request[2] < 0x20u) return 0;
    memset(answer, 0, HOST_LINK_HEAD_BYTES);
    answer[2] = (uint8_t)(request[2] ^ 0x40u);
    answer[3] = 1;
    answer[4] = request[4];
    return 1;
}

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  FAIL: " __VA_ARGS__); printf("\n"); failures++; } } while (0)

int main(void)
{
    const HostLinkOps ops = {d_transfer, d_ready, d_dispatch};
    for (unsigned s = 0; s < HL_SCENARIO_COUNT; s++) {
        const hl_scenario_t *sc = &hl_scenarios[s];
        HostLinkConfig cfg = {hl_configs[sc->cfg].idle_interval_ms, hl_configs[sc->cfg].min_gap_ms, hl_configs[sc->cfg].max_retries, hl_configs[sc->cfg].down_after_failures,
                              hl_configs[sc->cfg].require_seal, hl_configs[sc->cfg].ready_known};
        HostLink_t link;
        Double d;
        memset(&d, 0, sizeof d);
        if (!HostLink_Init(&link, &cfg, &ops, &d)) {
            printf("  FAIL: %s: the configuration was refused\n", sc->name);
            failures++;
            continue;
        }
        for (unsigned i = 0; i < sc->count; i++) {
            const hl_in_t *in = &sc->in[i];
            const hl_out_t *want = &sc->out[i];
            d.step = in;
            if (in->stage_type) {
                uint8_t frame[HOST_LINK_HEAD_BYTES] = {0, 0, in->stage_type, 1, in->stage_byte, 0, 0, 0, 0, 0, 0, 0};
                (void)HostLink_Stage(&link, frame);
            }
            const HostLinkPoll got = HostLink_Poll(&link, in->t);
            const HostLinkCounters *c = &link.counters;
            const uint8_t rx = got == HOST_LINK_POLL_DONE ? link.last_rx_verdict : 0;
            const uint8_t tx_type = (got == HOST_LINK_POLL_DONE || got == HOST_LINK_POLL_XFER_FAIL) ? link.last_tx_type : 0;
            const uint8_t tx_seq = (got == HOST_LINK_POLL_DONE || got == HOST_LINK_POLL_XFER_FAIL) ? link.last_tx_seq : 0;
            if (got != want->poll || rx != want->rx || tx_type != want->tx_type || tx_seq != want->tx_seq || c->transactions != want->transactions || c->dispatched != want->dispatched ||
                c->duplicates != want->duplicates || c->bad_crc != want->bad_crc || c->unsealed != want->unsealed || c->idle_rx != want->idle_rx || c->silence != want->silence ||
                c->transfer_failures != want->transfer_failures || c->retries != want->retries || c->dropped != want->dropped || link.down != want->down ||
                link.staged_valid != want->staged) {
                printf("  FAIL: %s step %u: poll %u rx %u tx %02x/%u tr %u disp %u dup %u bad %u uns %u idle %u sil %u xf %u ret %u drop %u down %u staged %u\n", sc->name, i, (unsigned)got,
                       (unsigned)rx, (unsigned)tx_type, (unsigned)tx_seq, (unsigned)c->transactions, (unsigned)c->dispatched, (unsigned)c->duplicates, (unsigned)c->bad_crc,
                       (unsigned)c->unsealed, (unsigned)c->idle_rx, (unsigned)c->silence, (unsigned)c->transfer_failures, (unsigned)c->retries, (unsigned)c->dropped, (unsigned)link.down,
                       (unsigned)link.staged_valid);
                failures++;
                break;
            }
            if (got == HOST_LINK_POLL_DONE || got == HOST_LINK_POLL_XFER_FAIL) {
                CHECK(d.sent_sealed_ok, "%s step %u: the frame that went out is not sealed", sc->name, i);
            }
        }
    }

    /* a configuration that cannot work is refused, and so is a missing operation */
    {
        HostLink_t link;
        Double d;
        memset(&d, 0, sizeof d);
        const HostLinkConfig good = {50, 2, 3, 5, 1, 0};
        HostLinkConfig bad = good;
        HostLinkOps no_transfer = {0, d_ready, d_dispatch}, no_ready = {d_transfer, 0, d_dispatch};
        CHECK(HostLink_Init(&link, &good, &ops, &d), "the good configuration is accepted");
        bad = good;
        bad.idle_interval_ms = 0;
        CHECK(!HostLink_Init(&link, &bad, &ops, &d), "an idle interval of 0");
        bad = good;
        bad.idle_interval_ms = 10001;
        CHECK(!HostLink_Init(&link, &bad, &ops, &d), "an idle interval over 10 s");
        bad = good;
        bad.min_gap_ms = 51;
        CHECK(!HostLink_Init(&link, &bad, &ops, &d), "a gap above the idle interval");
        bad = good;
        bad.max_retries = 11;
        CHECK(!HostLink_Init(&link, &bad, &ops, &d), "11 retries");
        bad = good;
        bad.down_after_failures = 0;
        CHECK(!HostLink_Init(&link, &bad, &ops, &d), "down after 0 failures");
        bad = good;
        bad.require_seal = 2;
        CHECK(!HostLink_Init(&link, &bad, &ops, &d), "a seal flag of 2");
        CHECK(!HostLink_Init(&link, &good, &no_transfer, &d), "no transfer operation");
        CHECK(HostLink_Init(&link, &good, &no_ready, &d), "no READY operation is fine when the line is not wired");
        bad = good;
        bad.ready_known = 1;
        CHECK(!HostLink_Init(&link, &bad, &no_ready, &d), "no READY operation when the line is wired");
    }
    /* the slot holds one frame */
    {
        HostLink_t link;
        Double d;
        const hl_in_t quiet = {0, 0, 0, 0, 0, 0, 1, 0, 0};
        const HostLinkConfig good = {50, 2, 3, 5, 1, 0};
        const uint8_t frame[HOST_LINK_HEAD_BYTES] = {0, 0, 0x40, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        memset(&d, 0, sizeof d);
        d.step = &quiet;
        CHECK(HostLink_Init(&link, &good, &ops, &d), "init");
        CHECK(HostLink_Stage(&link, frame) == 1, "the first frame is staged");
        CHECK(HostLink_Stage(&link, frame) == 0, "the second is refused while the slot is taken");
    }
    if (failures) {
        printf("test_host_link_master: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_host_link_master: all %u scenarios passed\n", (unsigned)HL_SCENARIO_COUNT);
    return 0;
}
