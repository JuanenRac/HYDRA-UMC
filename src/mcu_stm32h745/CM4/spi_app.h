/* =============================================================================
 * HYDRA-UMC - src/mcu_stm32h745/CM4/spi_app.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * The SPI slave service of the APPLICATION of the CM4 (HAL-free, host-testable). Until now only the bootloader answered the SPI link; once the
 * application started, the CM5 had nobody to talk to. This file is the logic of that service; SpiApp.c is the driver (SPI1 in slave mode and one
 * task) and tests/test_spi_app.c runs it on the PC against the real state machine of the brain (estop_brain.h) with a CM5 played by the test.
 *
 * What it does with a received 128-byte frame (the wire format of docs/SPI_FRAME_INTEGRITY.md):
 *   - the seal is checked (spi_frame_crc.h): a damaged frame is dropped and counted, a repeated sequence is a retry and is not run twice, a
 *     frame without the seal is accepted unless the build asks for it (HYDRA_SPI_REQUIRE_CRC);
 *   - a frame of the tier SELF that is one of the commands of spi_system.h goes to SpiSys_Handle; every other frame (relay to the stack, to the
 *     CM7, update frames) is not answered by the application: an update starts in the bootloader, never here;
 *   - the answer of a command is sealed and waits for the NEXT transaction (SPI moves data only when the master clocks it), as in the bootloader.
 *
 * The SPI task never touches the emergency stop. It leaves requests (stop with a reason, release, slot scan) that the STACK A task takes, because
 * every call of KinematicBrainCan_Estop* is made from that task only; the STACK A task in turn hands over the state of the stop and the counters of
 * the bus, and the verdicts of a scan. Each of those fields has one writer. Bytes only, so a read in the middle of a write is harmless.
 *
 * Switches (0 by default, so the firmware does not start the service until a build asks for it): HYDRA_SPI_APP. It also needs HYDRA_SPI_SYSTEM
 * (telemetry and probe) and, for stop and release, HYDRA_SPI_STOP_COMMAND. HYDRA_SPI_ZONES_HEARTBEAT makes the application stop the stack when the
 * heartbeat of the safety-zones chain (type 0x26, sent by the CM5 only while the chain is healthy) has come and then stops coming; it never trips
 * before the first heartbeat, so a machine without that service is not affected. docs/SPI_SYSTEM.md is the contract.
 * ========================================================================== */
#ifndef HYDRA_UMC_SPI_APP_H
#define HYDRA_UMC_SPI_APP_H

#include <stdint.h>
#include <string.h>

#include "spi_frame_crc.h"
#include "spi_system.h"

#ifndef HYDRA_SPI_APP
#define HYDRA_SPI_APP 0
#endif
#ifndef HYDRA_SPI_REQUIRE_CRC
#define HYDRA_SPI_REQUIRE_CRC 0
#endif
#ifndef HYDRA_SPI_ZONES_HEARTBEAT
#define HYDRA_SPI_ZONES_HEARTBEAT 0
#endif
#ifndef HYDRA_SPI_ZONES_MAX_AGE_MS
#define HYDRA_SPI_ZONES_MAX_AGE_MS 1000u /* the oldest heartbeat that still counts; the safety-zones service sends one every 100 ms */
#endif
#define SPI_APP_ZONES_REASON 0x04u /* ESTOP_REASON_SAFETY_ZONE of estop_can.h (tests/test_spi_app.py compares the two) */

#define SPI_APP_FRAME_SIZE 128u
#define SPI_APP_TARGET_SELF 0u
#define SPI_APP_IDLE_TYPE 0xFFu /* the frame that goes out when there is nothing to say (the master ignores it) */

typedef struct {
    SpiSysConfig_t config;
    uint8_t require_crc;
    /* the sequence state of the link (spi_frame_crc.h) */
    uint8_t tx_seq;
    uint8_t rx_have_last;
    uint8_t rx_last;
    uint32_t crc_errors;
    uint32_t retries;
    uint32_t seq_gaps;
    uint32_t unsealed;
    uint32_t commands_run;
    uint32_t frames_ignored; /* sealed and fine, but not a command of this service */
    /* what goes out on the next transaction */
    uint8_t tx_wire[SPI_APP_FRAME_SIZE];
    /* written by the STACK A task, read here */
    volatile uint8_t estop_latched;
    volatile uint8_t can_tx_errors;
    volatile uint8_t can_rx_errors;
    volatile uint8_t link_ok;
    /* requests left by the SPI task for the STACK A task (one writer each side: set here, cleared by the Take functions) */
    volatile uint8_t trip_pending;
    volatile uint8_t trip_reason;
    volatile uint8_t release_pending;
    volatile uint8_t scan_pending;
    volatile uint8_t scan_id;
    /* the answer of a scan, written by the STACK A task */
    volatile uint8_t scan_done;
    volatile uint8_t scan_done_id;
    volatile uint8_t scan_verdicts[8];
    /* the heartbeat of the safety-zones chain: the SPI task counts them, the STACK A task times them (it owns the clock) */
    volatile uint32_t zones_beat;
    uint32_t zones_beat_seen;
    uint32_t zones_last_ms;
    uint8_t zones_armed;
} SpiApp_t;

static inline void SpiApp_SealIdle(SpiApp_t *app)
{
    memset(app->tx_wire, 0xFF, SPI_APP_FRAME_SIZE);
    app->tx_wire[0] = SPI_APP_TARGET_SELF;
    app->tx_wire[1] = 0u;
    app->tx_wire[2] = SPI_APP_IDLE_TYPE;
    app->tx_wire[3] = 0u;
    memset(&app->tx_wire[4], 0, 8);
    SpiWire_Seal(app->tx_wire, app->tx_seq++);
}

static inline void SpiApp_Init(SpiApp_t *app, const SpiSysConfig_t *config, uint8_t require_crc)
{
    memset(app, 0, sizeof *app);
    app->config = *config;
    app->require_crc = require_crc;
    app->link_ok = 1u;
    app->estop_latched = 1u; /* the brain starts latched (estop_brain.h) */
    SpiApp_SealIdle(app);
}

static inline void SpiApp_StageReply(SpiApp_t *app, uint8_t frame_type, const uint8_t *data, uint8_t dlc)
{
    memset(app->tx_wire, 0xFF, SPI_APP_FRAME_SIZE);
    app->tx_wire[0] = SPI_APP_TARGET_SELF;
    app->tx_wire[1] = 0u;
    app->tx_wire[2] = frame_type;
    app->tx_wire[3] = dlc;
    memset(&app->tx_wire[4], 0, 8);
    memcpy(&app->tx_wire[4], data, dlc > 8u ? 8u : dlc);
    SpiWire_Seal(app->tx_wire, app->tx_seq++);
}

static inline SpiSysSnapshot_t SpiApp_Snapshot(const SpiApp_t *app)
{
    SpiSysSnapshot_t snap;
    memset(&snap, 0, sizeof snap);
    snap.crc_errors = app->crc_errors;
    snap.retries = app->retries;
    snap.seq_gaps = app->seq_gaps;
    snap.can_tx_errors = app->can_tx_errors;
    snap.can_rx_errors = app->can_rx_errors;
    snap.state = SPI_SYS_STATE_APPLICATION;
    snap.link_ok = app->link_ok;
    snap.estop_latched = app->estop_latched;
    for (int slot = 0; slot < 8; slot++) snap.scan_verdicts[slot] = app->scan_verdicts[slot];
    return snap;
}

/* One frame received from the CM5. Returns 1 when it was a command that this service ran, 0 for everything else (damaged, a retry, refused for
 * the missing seal, not a command, a switch that is off). Call it after every transaction; then clock the next one out of tx_wire. */
static inline uint8_t SpiApp_OnReceive(SpiApp_t *app, const uint8_t rx_wire[SPI_APP_FRAME_SIZE])
{
    uint8_t seq = 0u;
    switch (SpiWire_Judge(rx_wire, app->rx_have_last, app->rx_last, &seq)) {
    case SPI_ACCEPT_BAD_CRC:
        app->crc_errors++;
        return 0u;
    case SPI_ACCEPT_DUPLICATE:
        app->retries++;
        return 0u;
    case SPI_ACCEPT_UNSEALED:
        app->unsealed++;
        if (app->require_crc) return 0u;
        break;
    default:
        if (app->rx_have_last && (uint8_t)(seq - app->rx_last) != 1u) app->seq_gaps++;
        app->rx_have_last = 1u;
        app->rx_last = seq;
        break;
    }
    const uint8_t tier = rx_wire[0];
    const uint8_t frame_type = rx_wire[2];
    uint8_t dlc = rx_wire[3];
    if (tier != SPI_APP_TARGET_SELF || !SpiSys_IsCommand(frame_type) || dlc > 8u) {
        app->frames_ignored++;
        return 0u;
    }
    const SpiSysSnapshot_t snap = SpiApp_Snapshot(app);
    uint8_t reply_type = 0u, reply[8], reply_dlc = 0u;
    const SpiSysAction action = SpiSys_Handle(&app->config, &snap, frame_type, &rx_wire[4], dlc, &reply_type, reply, &reply_dlc);
    switch (action) {
    case SPI_SYS_ACT_REPLY:
        SpiApp_StageReply(app, reply_type, reply, reply_dlc);
        break;
    case SPI_SYS_ACT_TRIP:
        app->trip_reason = reply[0];
        app->trip_pending = 1u;
        break;
    case SPI_SYS_ACT_REQUEST_RELEASE:
        app->release_pending = 1u;
        break;
    case SPI_SYS_ACT_START_SCAN:
        app->scan_id = reply[0];
        app->scan_done = 0u;
        app->scan_pending = 1u;
        break;
    case SPI_SYS_ACT_ZONES_HEARTBEAT:
        app->zones_beat = app->zones_beat + 1u;
        break;
    default:
        app->frames_ignored++;
        return 0u;
    }
    app->commands_run++;
    return 1u;
}

/* The SPI task calls this between transactions: a finished scan becomes the answer of the probe. */
static inline void SpiApp_Service(SpiApp_t *app)
{
    if (app->scan_done) {
        uint8_t answer_type, answer[8], answer_dlc;
        SpiSysSnapshot_t snap = SpiApp_Snapshot(app);
        SpiSys_ProbeAnswer(&snap, app->scan_done_id, &answer_type, answer, &answer_dlc);
        app->scan_done = 0u;
        SpiApp_StageReply(app, answer_type, answer, answer_dlc);
    }
}

/* The STACK A task side. Each Take returns 1 once per request. */
static inline uint8_t SpiApp_TakeTrip(SpiApp_t *app, uint8_t *reason)
{
    if (!app->trip_pending) return 0u;
    *reason = app->trip_reason;
    app->trip_pending = 0u;
    return 1u;
}

static inline uint8_t SpiApp_TakeRelease(SpiApp_t *app)
{
    if (!app->release_pending) return 0u;
    app->release_pending = 0u;
    return 1u;
}

static inline uint8_t SpiApp_TakeScan(SpiApp_t *app, uint8_t *scan_id)
{
    if (!app->scan_pending) return 0u;
    *scan_id = app->scan_id;
    app->scan_pending = 0u;
    return 1u;
}

/* The heartbeat watchdog, from the STACK A task with its clock in milliseconds: 1 when a heartbeat came before and none has come for more than max_age_ms,
 * once per loss (the watchdog is armed again by the next heartbeat). The caller stops the stack with SPI_APP_ZONES_REASON. Never 1 before the first heartbeat. */
static inline uint8_t SpiApp_ZonesTick(SpiApp_t *app, uint32_t now_ms, uint32_t max_age_ms)
{
    const uint32_t beat = app->zones_beat;
    if (beat != app->zones_beat_seen) {
        app->zones_beat_seen = beat;
        app->zones_last_ms = now_ms;
        app->zones_armed = 1u;
        return 0u;
    }
    if (!app->zones_armed) return 0u;
    if ((uint32_t)(now_ms - app->zones_last_ms) > max_age_ms) {
        app->zones_armed = 0u;
        return 1u;
    }
    return 0u;
}

static inline void SpiApp_ScanDone(SpiApp_t *app, uint8_t scan_id, const uint8_t verdicts[8])
{
    for (int slot = 0; slot < 8; slot++) app->scan_verdicts[slot] = verdicts[slot];
    app->scan_done_id = scan_id;
    app->scan_done = 1u;
}

static inline void SpiApp_PublishBrainState(SpiApp_t *app, uint8_t estop_latched, uint8_t can_tx_errors, uint8_t can_rx_errors, uint8_t link_ok)
{
    app->estop_latched = estop_latched;
    app->can_tx_errors = can_tx_errors;
    app->can_rx_errors = can_rx_errors;
    app->link_ok = link_ok;
}

#endif /* HYDRA_UMC_SPI_APP_H */
