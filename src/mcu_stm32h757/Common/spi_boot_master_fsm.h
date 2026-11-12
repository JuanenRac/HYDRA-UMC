// =============================================================================
// HYDRA-UMC Kinematic Brain - state machine of the bootloader as SPI master (header-only, no HAL)
// Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
// GPL-3.0-or-later - see repo root LICENSE
//
// The C twin of SpiMasterBootloader.receive in tools/spi_boot_master.py: one whole attempt to receive a signed image from the host (the Jetson) over the SPI link on
// which this board is the master (docs/SPI_BOOT_MASTER.md). It moves frames with the code of spi_boot_master.h and touches the hardware only through the callbacks of
// SbmOps (the SPI1 transfer, the READY line, the random source, the staging slot and the verification), so the whole logic runs on the host with a double.
//
// Rules the machine keeps, the same as the model:
//   - the switch is OFF by default (HYDRA_BOOT_SPI_MASTER 0): with it off the machine does not call a single callback and the update goes on over CAN;
//   - an offer that does not fit (size, target, minimum bootloader) is refused before a page is asked for;
//   - a damaged answer, an answer to another request and a READY line that never rises are retried a few times and then the attempt FAILS;
//   - the staging slot is never marked valid by this machine until `verify` (the chain of trust of the bootloader: signature, CRC, hardware id, link address, vectors,
//     anti-rollback and the signed downgrade token) has accepted the whole image; on any other end the slot is invalidated;
//   - after an accepted image the DONE frame is sent once, and a host that does not hear it does not undo the staging.
// Nothing here has run on a board; the SPI1 driver that fills SbmOps for the real peripheral is a separate step.
// =============================================================================
#ifndef HYDRA_UMC_SPI_BOOT_MASTER_FSM_H
#define HYDRA_UMC_SPI_BOOT_MASTER_FSM_H

#include "spi_boot_master.h"

#ifndef HYDRA_BOOT_SPI_MASTER
#define HYDRA_BOOT_SPI_MASTER 0
#endif
#define SBM_MAX_RETRIES 3u

typedef enum {
    SBM_RESULT_DISABLED = 0,
    SBM_RESULT_ACCEPTED = 1,
    SBM_RESULT_REFUSED = 2,
    SBM_RESULT_FAILED = 3
} SbmResult;

typedef enum {
    SBM_WHY_NONE = 0,
    SBM_WHY_SWITCH_OFF,
    SBM_WHY_HOST_HAS_NOTHING,
    SBM_WHY_NOT_AN_OFFER,
    SBM_WHY_OFFER_BAD_SIZE,
    SBM_WHY_OFFER_WRONG_TARGET,
    SBM_WHY_OFFER_BOOTLOADER_TOO_OLD,
    SBM_WHY_PAGE_NOT_AS_ASKED,
    SBM_WHY_STAGE_WRITE,
    SBM_WHY_VERIFY,
    SBM_WHY_LINK
} SbmWhy;

typedef struct {
    /* one transaction: send tx, read the answer into rx (at most rx_cap bytes) and give its length; 0 when the peripheral failed */
    int (*transfer)(void *ctx, const uint8_t *tx, uint32_t tx_len, uint8_t *rx, uint32_t rx_cap, uint32_t *rx_len);
    int (*ready)(void *ctx);                                  /* the READY line of the host: 1 when it has its answer */
    void (*nonce)(void *ctx, uint8_t out[SBM_NONCE_BYTES]);  /* the random source of the bootloader, one nonce per attempt */
    void (*stage_invalidate)(void *ctx);                      /* the staging slot holds nothing usable */
    int (*stage_write)(void *ctx, uint32_t offset, const uint8_t *data, uint32_t length); /* 1 on success */
    /* the chain of trust over the whole staged image; `token` is the signed downgrade token (token_length 0: none). 1: accepted */
    int (*verify)(void *ctx, uint32_t size, const uint8_t nonce[SBM_NONCE_BYTES], const uint8_t *token, uint32_t token_length);
    void (*stage_mark_valid)(void *ctx, const uint16_t version[3]);
} SbmOps;

typedef struct {
    int enabled;                 /* the configuration switch; the default is HYDRA_BOOT_SPI_MASTER */
    uint32_t hw_id;
    uint32_t link_address;
    uint16_t installed_version[3];
    uint16_t bootloader_version[3];
    const char *target;          /* the name of this board in the offer, e.g. "H757-CM7" */
    uint32_t max_image_bytes;
    uint32_t ready_timeout_polls;
    int token_wanted;
} SbmConfig;

typedef struct {
    SbmResult result;
    SbmWhy why;
    uint32_t pages;
    uint32_t frames;
    uint16_t sequence;
    uint8_t nonce[SBM_NONCE_BYTES];
    uint8_t tx[SBM_MAX_FRAME];
    uint8_t rx[SBM_MAX_FRAME];
} SbmSession;

/* One request and its answer, with the retries a damaged answer deserves. 1 and the answer in *type / *payload / *length; 0 when the link cannot be used. */
static inline int Sbm_Exchange(SbmSession *s, const SbmConfig *cfg, const SbmOps *ops, void *ctx, uint8_t kind, const uint8_t *payload, uint16_t payload_length,
                               uint8_t *type, const uint8_t **answer, uint16_t *answer_length)
{
    for (uint32_t attempt = 0; attempt < SBM_MAX_RETRIES; attempt++) {
        s->sequence = (uint16_t)(s->sequence + 1u);
        const uint16_t expected = s->sequence;
        s->frames++;
        const uint32_t tx_length = Sbm_Encode(s->tx, kind, expected, payload, payload_length);
        uint32_t rx_length = 0;
        if (tx_length == 0u || !ops->transfer(ctx, s->tx, tx_length, s->rx, (uint32_t)sizeof s->rx, &rx_length)) continue;
        uint32_t polls = 0;
        while (!ops->ready(ctx)) {
            if (++polls > cfg->ready_timeout_polls) return 0; /* the host never raised READY: no retry would help */
        }
        uint16_t sequence = 0;
        if (Sbm_Decode(s->rx, rx_length, 1, type, &sequence, answer, answer_length) != SBM_OK) continue;
        if (sequence != expected) continue; /* the answer belongs to another request */
        return 1;
    }
    return 0;
}

static inline SbmResult Sbm_Finish(SbmSession *s, const SbmOps *ops, void *ctx, SbmResult result, SbmWhy why)
{
    if (result != SBM_RESULT_ACCEPTED) ops->stage_invalidate(ctx);
    s->result = result;
    s->why = why;
    return result;
}

/* One whole attempt. The session holds the counters and the buffers; the result says how it ended. */
static inline SbmResult Sbm_Receive(SbmSession *s, const SbmConfig *cfg, const SbmOps *ops, void *ctx)
{
    s->pages = 0;
    s->frames = 0;
    s->sequence = 0;
    s->why = SBM_WHY_NONE;
    if (!cfg->enabled) {
        s->result = SBM_RESULT_DISABLED;
        s->why = SBM_WHY_SWITCH_OFF;
        return s->result; /* not a single callback: the update goes on over CAN */
    }
    ops->stage_invalidate(ctx);
    ops->nonce(ctx, s->nonce);

    uint8_t payload[36];
    Sbm_Put32(payload, cfg->hw_id);
    Sbm_Put32(payload + 4, cfg->link_address);
    for (uint32_t i = 0; i < 3u; i++) Sbm_Put16(payload + 8 + 2u * i, cfg->installed_version[i]);
    for (uint32_t i = 0; i < SBM_NONCE_BYTES; i++) payload[14 + i] = s->nonce[i];
    for (uint32_t i = 0; i < 3u; i++) Sbm_Put16(payload + 30 + 2u * i, cfg->bootloader_version[i]);

    uint8_t type = 0;
    const uint8_t *body = 0;
    uint16_t length = 0;
    if (!Sbm_Exchange(s, cfg, ops, ctx, SBM_T_HELLO, payload, (uint16_t)sizeof payload, &type, &body, &length)) return Sbm_Finish(s, ops, ctx, SBM_RESULT_FAILED, SBM_WHY_LINK);
    if (type == SBM_T_REFUSE) return Sbm_Finish(s, ops, ctx, SBM_RESULT_REFUSED, SBM_WHY_HOST_HAS_NOTHING);
    SbmOffer offer;
    if (type != SBM_T_OFFER || !Sbm_ParseOffer(body, length, &offer)) return Sbm_Finish(s, ops, ctx, SBM_RESULT_FAILED, SBM_WHY_NOT_AN_OFFER);
    /* the checks that refuse before a byte is fetched; none of them replaces the verification below */
    switch (Sbm_JudgeOffer(&offer, cfg->max_image_bytes, cfg->target, cfg->bootloader_version)) {
    case SBM_OFFER_OK: break;
    case SBM_OFFER_BAD_SIZE: return Sbm_Finish(s, ops, ctx, SBM_RESULT_REFUSED, SBM_WHY_OFFER_BAD_SIZE);
    case SBM_OFFER_WRONG_TARGET: return Sbm_Finish(s, ops, ctx, SBM_RESULT_REFUSED, SBM_WHY_OFFER_WRONG_TARGET);
    default: return Sbm_Finish(s, ops, ctx, SBM_RESULT_REFUSED, SBM_WHY_OFFER_BOOTLOADER_TOO_OLD);
    }

    uint32_t received = 0;
    while (received < offer.size) {
        const uint32_t want = (offer.size - received) < SBM_PAGE_BYTES ? (offer.size - received) : SBM_PAGE_BYTES;
        uint8_t request[8];
        Sbm_Put32(request, received);
        Sbm_Put32(request + 4, want);
        if (!Sbm_Exchange(s, cfg, ops, ctx, SBM_T_GET_PAGE, request, (uint16_t)sizeof request, &type, &body, &length)) return Sbm_Finish(s, ops, ctx, SBM_RESULT_FAILED, SBM_WHY_LINK);
        if (type != SBM_T_PAGE || length != 4u + want || Sbm_Get32(body) != received) return Sbm_Finish(s, ops, ctx, SBM_RESULT_FAILED, SBM_WHY_PAGE_NOT_AS_ASKED);
        if (!ops->stage_write(ctx, received, body + 4, want)) return Sbm_Finish(s, ops, ctx, SBM_RESULT_FAILED, SBM_WHY_STAGE_WRITE);
        received += want;
        s->pages++;
    }

    const uint8_t *token = 0;
    uint32_t token_length = 0;
    if (cfg->token_wanted) {
        if (!Sbm_Exchange(s, cfg, ops, ctx, SBM_T_GET_TOKEN, 0, 0, &type, &body, &length)) return Sbm_Finish(s, ops, ctx, SBM_RESULT_FAILED, SBM_WHY_LINK);
        if (type == SBM_T_TOKEN) {
            token = body;
            token_length = length;
        }
    }
    if (!ops->verify(ctx, offer.size, s->nonce, token, token_length)) return Sbm_Finish(s, ops, ctx, SBM_RESULT_REFUSED, SBM_WHY_VERIFY);
    ops->stage_mark_valid(ctx, offer.version); /* only here: the whole image has been verified */
    (void)Sbm_Exchange(s, cfg, ops, ctx, SBM_T_DONE, 0, 0, &type, &body, &length); /* the host not hearing the end does not undo the staging */
    return Sbm_Finish(s, ops, ctx, SBM_RESULT_ACCEPTED, SBM_WHY_NONE);
}

#endif /* HYDRA_UMC_SPI_BOOT_MASTER_FSM_H */
