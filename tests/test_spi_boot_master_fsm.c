/* =============================================================================
 * HYDRA-UMC Firmware - host test of spi_boot_master_fsm.h (the state machine of the bootloader as SPI master)
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * The Jetson is a double that serves an image blob page by page and can be made to misbehave; the staging slot and the verification are doubles too. The frames of a
 * clean session are compared with the golden ones that the Python model builds (tests/spi_boot_master_vectors.h). Nothing here touches the SPI peripheral.
 */
#include <stdio.h>
#include <string.h>
#include "spi_boot_master_fsm.h"
#include "spi_boot_master_vectors.h"

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } } while (0)

#define BLOB_MAX 2048u

typedef struct {
    /* the host double */
    uint8_t blob[BLOB_MAX];
    uint32_t size;               /* the size it offers */
    uint16_t version[3];
    uint16_t min_bootloader[3];
    const char *target;
    int refuse;                  /* answer HELLO with REFUSE */
    int corrupt_offset;          /* corrupt the PAGE answer for this offset (-1: none) ... */
    unsigned corrupt_times;      /* ... this many times */
    int wrong_sequence;          /* answer with the sequence of another request */
    int never_ready;             /* the READY line stays low */
    int transfer_fails;          /* the peripheral fails */
    int token_present;
    /* what the machine did */
    uint8_t requests[16][SBM_MAX_FRAME];
    uint32_t request_bytes[16];
    unsigned request_count;
    unsigned transfers;
    unsigned ready_calls;
    unsigned nonce_calls;
    /* the staging double */
    uint8_t staged[BLOB_MAX];
    uint32_t staged_bytes;
    int valid;
    unsigned invalidated;
    unsigned marked_valid;
    unsigned verify_calls;
    int verify_result;
    uint32_t verify_size;
    uint32_t verify_token_length;
    int stage_write_fails;
} World;

static int op_transfer(void *ctx, const uint8_t *tx, uint32_t tx_len, uint8_t *rx, uint32_t rx_cap, uint32_t *rx_len)
{
    World *w = (World *)ctx;
    w->transfers++;
    if (w->transfer_fails) return 0;
    if (w->request_count < 16u) {
        memcpy(w->requests[w->request_count], tx, tx_len);
        w->request_bytes[w->request_count] = tx_len;
        w->request_count++;
    }
    uint8_t type = 0;
    uint16_t sequence = 0, length = 0;
    const uint8_t *payload = 0;
    if (Sbm_Decode(tx, tx_len, 0, &type, &sequence, &payload, &length) != SBM_OK) return 0;
    const uint16_t answer_sequence = w->wrong_sequence ? (uint16_t)(sequence + 1u) : sequence;
    uint8_t body[SBM_MAX_PAYLOAD];
    uint32_t body_length = 0;
    uint8_t answer = SBM_T_REFUSE;
    if (type == SBM_T_HELLO) {
        if (w->refuse) {
            answer = SBM_T_REFUSE;
        } else {
            answer = SBM_T_OFFER;
            Sbm_Put32(body, w->size);
            for (uint32_t i = 0; i < 3u; i++) {
                Sbm_Put16(body + 4 + 2u * i, w->version[i]);
                Sbm_Put16(body + 10 + 2u * i, w->min_bootloader[i]);
            }
            memset(body + 16, 0, SBM_TARGET_BYTES);
            for (uint32_t i = 0; i < SBM_TARGET_BYTES && w->target[i] != '\0'; i++) body[16 + i] = (uint8_t)w->target[i];
            body_length = SBM_OFFER_BYTES;
        }
    } else if (type == SBM_T_GET_PAGE) {
        const uint32_t offset = Sbm_Get32(payload), want = Sbm_Get32(payload + 4);
        answer = SBM_T_PAGE;
        Sbm_Put32(body, offset);
        memcpy(body + 4, w->blob + offset, want);
        body_length = 4u + want;
    } else if (type == SBM_T_GET_TOKEN) {
        answer = SBM_T_TOKEN;
        body_length = w->token_present ? 64u : 0u;
        for (uint32_t i = 0; i < body_length; i++) body[i] = (uint8_t)(0xA0u + i);
    }
    const uint32_t n = Sbm_Encode(rx, answer, answer_sequence, body, (uint16_t)body_length);
    CHECK(n <= rx_cap, "the answer fits the buffer");
    if (type == SBM_T_GET_PAGE && w->corrupt_offset >= 0 && Sbm_Get32(payload) == (uint32_t)w->corrupt_offset && w->corrupt_times > 0u) {
        w->corrupt_times--;
        rx[12] ^= 0x40u; /* a bit flipped in the payload: the CRC no longer matches */
    }
    *rx_len = n;
    return 1;
}

static int op_ready(void *ctx) { World *w = (World *)ctx; w->ready_calls++; return !w->never_ready; }
static void op_nonce(void *ctx, uint8_t out[SBM_NONCE_BYTES]) { World *w = (World *)ctx; w->nonce_calls++; for (uint32_t i = 0; i < SBM_NONCE_BYTES; i++) out[i] = (uint8_t)i; }
static void op_invalidate(void *ctx) { World *w = (World *)ctx; w->invalidated++; w->valid = 0; w->staged_bytes = 0; }
static int op_stage_write(void *ctx, uint32_t offset, const uint8_t *data, uint32_t length)
{
    World *w = (World *)ctx;
    if (w->stage_write_fails || offset + length > BLOB_MAX) return 0;
    memcpy(w->staged + offset, data, length);
    if (offset + length > w->staged_bytes) w->staged_bytes = offset + length;
    return 1;
}
static int op_verify(void *ctx, uint32_t size, const uint8_t nonce[SBM_NONCE_BYTES], const uint8_t *token, uint32_t token_length)
{
    World *w = (World *)ctx;
    (void)nonce;
    (void)token;
    w->verify_calls++;
    w->verify_size = size;
    w->verify_token_length = token_length;
    return w->verify_result;
}
static void op_mark_valid(void *ctx, const uint16_t version[3]) { World *w = (World *)ctx; (void)version; w->marked_valid++; w->valid = 1; }

static const SbmOps OPS = {op_transfer, op_ready, op_nonce, op_invalidate, op_stage_write, op_verify, op_mark_valid};

static void world_init(World *w, uint32_t size)
{
    memset(w, 0, sizeof *w);
    w->size = size;
    w->version[1] = 1; w->version[2] = 9; /* 0.1.9 */
    w->target = "H757-CM7";
    w->corrupt_offset = -1;
    w->verify_result = 1;
    for (uint32_t i = 0; i < BLOB_MAX; i++) w->blob[i] = (uint8_t)(i * 7u + 3u);
}

static void config_init(SbmConfig *c)
{
    memset(c, 0, sizeof *c);
    c->enabled = 1;
    c->hw_id = SBM_HELLO_HW_ID;
    c->link_address = SBM_HELLO_LINK;
    memcpy(c->installed_version, SBM_HELLO_INSTALLED, sizeof c->installed_version);
    memcpy(c->bootloader_version, SBM_HELLO_BOOTLOADER, sizeof c->bootloader_version);
    c->target = SBM_JUDGE_BOARD_TARGET;
    c->max_image_bytes = SBM_JUDGE_MAX_IMAGE_BYTES;
    c->ready_timeout_polls = 8;
}

static uint32_t from_hex(const char *hex, uint8_t *out, uint32_t capacity)
{
    uint32_t n = 0;
    while (hex[2u * n] != '\0' && n < capacity) {
        unsigned v = 0;
        sscanf(hex + 2u * n, "%2x", &v);
        out[n++] = (uint8_t)v;
    }
    return n;
}

static SbmSession session;

int main(void)
{
    World w;
    SbmConfig cfg;

    /* the switch is off by default: not a single callback is called */
    CHECK(HYDRA_BOOT_SPI_MASTER == 0, "the build switch is off by default");
    world_init(&w, SBM_SESSION_SIZE);
    config_init(&cfg);
    cfg.enabled = HYDRA_BOOT_SPI_MASTER;
    CHECK(Sbm_Receive(&session, &cfg, &OPS, &w) == SBM_RESULT_DISABLED, "off: disabled");
    CHECK(session.why == SBM_WHY_SWITCH_OFF, "off: the reason");
    CHECK(w.transfers == 0 && w.ready_calls == 0 && w.nonce_calls == 0 && w.invalidated == 0 && w.verify_calls == 0 && w.marked_valid == 0, "off: no callback is called");

    /* a clean session: the requests are the golden ones of the model, the image is staged, verified once, and only then valid */
    world_init(&w, SBM_SESSION_SIZE);
    config_init(&cfg);
    CHECK(Sbm_Receive(&session, &cfg, &OPS, &w) == SBM_RESULT_ACCEPTED, "clean: accepted");
    CHECK(session.pages == 3u && session.frames == SBM_SESSION_REQUESTS, "clean: three pages and five frames");
    CHECK(w.request_count == SBM_SESSION_REQUESTS, "clean: five requests");
    for (unsigned i = 0; i < SBM_SESSION_REQUESTS && i < w.request_count; i++) {
        uint8_t want[SBM_MAX_FRAME];
        const uint32_t want_bytes = from_hex(SBM_SESSION_FRAMES[i], want, sizeof want);
        CHECK(w.request_bytes[i] == want_bytes && memcmp(w.requests[i], want, want_bytes) == 0, "clean: the request is the golden frame of the model");
    }
    CHECK(w.staged_bytes == SBM_SESSION_SIZE && memcmp(w.staged, w.blob, SBM_SESSION_SIZE) == 0, "clean: the staged image is the offered one");
    CHECK(w.verify_calls == 1 && w.verify_size == SBM_SESSION_SIZE && w.marked_valid == 1 && w.valid == 1, "clean: verified once, then valid");
    CHECK(w.verify_token_length == 0, "clean: no token asked for");

    /* a signed downgrade token is fetched and handed to the verification when it is wanted */
    world_init(&w, SBM_SESSION_SIZE);
    config_init(&cfg);
    cfg.token_wanted = 1;
    w.token_present = 1;
    CHECK(Sbm_Receive(&session, &cfg, &OPS, &w) == SBM_RESULT_ACCEPTED && w.verify_token_length == 64u, "token: handed to the verification");

    /* the verification refuses: the slot is never valid and is invalidated */
    world_init(&w, SBM_SESSION_SIZE);
    config_init(&cfg);
    w.verify_result = 0;
    CHECK(Sbm_Receive(&session, &cfg, &OPS, &w) == SBM_RESULT_REFUSED && session.why == SBM_WHY_VERIFY, "bad signature: refused");
    CHECK(w.marked_valid == 0 && w.valid == 0 && w.staged_bytes == 0, "bad signature: the slot is never valid and is empty");

    /* a damaged page is asked for again; three damaged answers end the attempt */
    world_init(&w, SBM_SESSION_SIZE);
    config_init(&cfg);
    w.corrupt_offset = 256;
    w.corrupt_times = 2;
    CHECK(Sbm_Receive(&session, &cfg, &OPS, &w) == SBM_RESULT_ACCEPTED && session.frames == SBM_SESSION_REQUESTS + 2u, "two damaged answers: retried and accepted");
    world_init(&w, SBM_SESSION_SIZE);
    config_init(&cfg);
    w.corrupt_offset = 256;
    w.corrupt_times = 3;
    CHECK(Sbm_Receive(&session, &cfg, &OPS, &w) == SBM_RESULT_FAILED && session.why == SBM_WHY_LINK, "three damaged answers: failed");
    CHECK(w.marked_valid == 0 && w.verify_calls == 0 && w.valid == 0, "three damaged answers: nothing verified, nothing valid");

    /* an answer for another request is retried and then fails */
    world_init(&w, SBM_SESSION_SIZE);
    config_init(&cfg);
    w.wrong_sequence = 1;
    CHECK(Sbm_Receive(&session, &cfg, &OPS, &w) == SBM_RESULT_FAILED && w.transfers == SBM_MAX_RETRIES, "wrong sequence: three tries, failed");

    /* the READY line never rises: no retry would help */
    world_init(&w, SBM_SESSION_SIZE);
    config_init(&cfg);
    w.never_ready = 1;
    CHECK(Sbm_Receive(&session, &cfg, &OPS, &w) == SBM_RESULT_FAILED && w.transfers == 1u && w.ready_calls == cfg.ready_timeout_polls + 1u, "READY never rises: failed after the polls");

    /* the peripheral fails */
    world_init(&w, SBM_SESSION_SIZE);
    config_init(&cfg);
    w.transfer_fails = 1;
    CHECK(Sbm_Receive(&session, &cfg, &OPS, &w) == SBM_RESULT_FAILED && w.transfers == SBM_MAX_RETRIES && w.valid == 0, "peripheral fails: failed");

    /* the host has nothing to offer */
    world_init(&w, SBM_SESSION_SIZE);
    config_init(&cfg);
    w.refuse = 1;
    CHECK(Sbm_Receive(&session, &cfg, &OPS, &w) == SBM_RESULT_REFUSED && session.why == SBM_WHY_HOST_HAS_NOTHING && w.request_count == 1u, "refuse: refused after the hello");

    /* an offer that does not fit is refused before a page is asked for */
    world_init(&w, 127u);
    config_init(&cfg);
    CHECK(Sbm_Receive(&session, &cfg, &OPS, &w) == SBM_RESULT_REFUSED && session.why == SBM_WHY_OFFER_BAD_SIZE && w.request_count == 1u, "size too small");
    world_init(&w, SBM_JUDGE_MAX_IMAGE_BYTES + 1u);
    config_init(&cfg);
    CHECK(Sbm_Receive(&session, &cfg, &OPS, &w) == SBM_RESULT_REFUSED && session.why == SBM_WHY_OFFER_BAD_SIZE && w.request_count == 1u, "size too big");
    world_init(&w, SBM_SESSION_SIZE);
    config_init(&cfg);
    w.target = "G474-CTRL";
    CHECK(Sbm_Receive(&session, &cfg, &OPS, &w) == SBM_RESULT_REFUSED && session.why == SBM_WHY_OFFER_WRONG_TARGET && w.request_count == 1u, "wrong target");
    world_init(&w, SBM_SESSION_SIZE);
    config_init(&cfg);
    w.min_bootloader[1] = 2; /* 0.2.0 */
    CHECK(Sbm_Receive(&session, &cfg, &OPS, &w) == SBM_RESULT_REFUSED && session.why == SBM_WHY_OFFER_BOOTLOADER_TOO_OLD && w.request_count == 1u, "bootloader too old");
    CHECK(w.verify_calls == 0 && w.marked_valid == 0, "refused offers verify and mark nothing");

    /* the staging slot cannot be written */
    world_init(&w, SBM_SESSION_SIZE);
    config_init(&cfg);
    w.stage_write_fails = 1;
    CHECK(Sbm_Receive(&session, &cfg, &OPS, &w) == SBM_RESULT_FAILED && session.why == SBM_WHY_STAGE_WRITE && w.valid == 0, "stage write fails: failed");

    /* an image of exactly one page and an image that is not a multiple of a page */
    world_init(&w, 256u);
    config_init(&cfg);
    CHECK(Sbm_Receive(&session, &cfg, &OPS, &w) == SBM_RESULT_ACCEPTED && session.pages == 1u, "one page");
    world_init(&w, 257u);
    config_init(&cfg);
    CHECK(Sbm_Receive(&session, &cfg, &OPS, &w) == SBM_RESULT_ACCEPTED && session.pages == 2u && w.staged_bytes == 257u, "257 bytes: two pages");

    if (failures == 0) printf("SPI_BOOT_MASTER_FSM=PASS\n");
    return failures == 0 ? 0 : 1;
}
