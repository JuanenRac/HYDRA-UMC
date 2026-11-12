/* =============================================================================
 * HYDRA-UMC Firmware - host logic tests for spi_app.h (the SPI slave service of the application of the CM4)
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * The test plays the CM5: it seals frames the way the bridge does, hands them to the service and reads what the service staged for the next
 * transaction. The stop, release and scan run against the real state machine of the brain (estop_brain.h) through the same few lines the driver
 * (SpiApp.c, SpiApp_Pump) runs. Built twice: SPI_APP_TEST_SWITCHES=1 (telemetry, probe, stop and release on) and 0 (every switch off: the
 * service answers and does nothing). The bytes marked VECTOR are the ones tools/spi_app.py builds; tests/test_spi_app.py compares them.
 * Evidence: simulator; the link at full rate is bench row 13 of docs/BENCH_TESTS.md.
 */
#include <stdio.h>
#include <string.h>

#include "estop_brain.h"
#include "fram_node.h"
#include "spi_app.h"

#ifndef SPI_APP_TEST_SWITCHES
#define SPI_APP_TEST_SWITCHES 1
#endif

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("  FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
            failures++; \
        } \
    } while (0)

static int failures;

/* A frame as the bridge of the CM5 lays it out and seals it. */
static void make_wire(uint8_t wire[SPI_APP_FRAME_SIZE], uint8_t tier, uint8_t type, const uint8_t *payload, uint8_t dlc, int seal, uint8_t seq)
{
    memset(wire, 0xFF, SPI_APP_FRAME_SIZE);
    wire[0] = tier;
    wire[1] = 0u;
    wire[2] = type;
    wire[3] = dlc;
    memset(&wire[4], 0, 8);
    if (dlc && payload) memcpy(&wire[4], payload, dlc > 8u ? 8u : dlc);
    if (seal) SpiWire_Seal(wire, seq);
}

static int same16(const uint8_t *wire, const uint8_t want[16])
{
    return memcmp(wire, want, 16) == 0;
}

/* The brain and the application as the firmware has them: the same few lines as SpiApp_Pump, with the brain of estop_brain.h and a clock. */
typedef struct {
    EstopBrain_t brain;
    uint32_t now;
    uint8_t expected_mask;
    uint8_t scan_runs;
} Rig_t;

static void rig_pump(Rig_t *rig, SpiApp_t *app)
{
    uint8_t reason = 0u, scan_id = 0u;
    if (SpiApp_TakeTrip(app, &reason)) EstopBrain_Trip(&rig->brain, rig->now, reason);
    if (SpiApp_TakeRelease(app)) (void)EstopBrain_RequestRelease(&rig->brain, rig->now, rig->expected_mask);
    if (SpiApp_TakeScan(app, &scan_id)) {
        const uint8_t verdicts[8] = {1u, 1u, 2u, 0u, 4u, 1u, 1u, 3u};
        rig->scan_runs++;
        SpiApp_ScanDone(app, scan_id, verdicts);
    }
    SpiApp_PublishBrainState(app, rig->brain.tripped, 0u, 0u, app->link_ok);
}

static SpiApp_t make_app(uint8_t telemetry, uint8_t stop, uint8_t require_crc)
{
    SpiApp_t app;
    const SpiSysConfig_t config = {telemetry, stop};
    SpiApp_Init(&app, &config, require_crc);
    return app;
}

static void run_with_switches_on(void)
{
    uint8_t wire[SPI_APP_FRAME_SIZE];
    const uint8_t query[1] = {0u};
    const uint8_t stop[5] = {'S', 'T', 'O', 'P', 9u};
    const uint8_t release[4] = {'R', 'E', 'L', 'S'};
    const uint8_t scan[1] = {0x2Au};
    const uint8_t zone[4] = {'Z', 'O', 'N', 'E'};

    /* power-up: the idle frame is sealed, of the type the master ignores, and the sequence has moved on */
    {
        SpiApp_t app = make_app(1u, 1u, 0u);
        /* VECTOR idle frame: 00 00 ff 00 00 00 00 00 00 00 00 00 a1 00 ba d7 */
        const uint8_t want[16] = {0x00, 0x00, 0xFF, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0xA1, 0x00, 0xBA, 0xD7};
        TEST_ASSERT(same16(app.tx_wire, want), "the idle frame at power-up");
        uint8_t seq = 99u;
        TEST_ASSERT(SpiWire_Check(app.tx_wire, &seq) == SPI_WIRE_SEALED_OK && seq == 0u && app.tx_seq == 1u, "sealed with sequence 0, the next one is 1");
        TEST_ASSERT(app.estop_latched == 1u && app.link_ok == 1u, "it starts latched with the link usable");
    }

    /* telemetry: the answer waits for the next transaction and carries the counters of the link */
    {
        SpiApp_t app = make_app(1u, 1u, 0u);
        make_wire(wire, 0u, SPI_SYS_TELEMETRY_QUERY, query, 1u, 1, 1u);
        /* VECTOR telemetry query: 00 00 22 01 00 00 00 00 00 00 00 00 a1 01 27 07 */
        const uint8_t sent[16] = {0x00, 0x00, 0x22, 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0xA1, 0x01, 0x27, 0x07};
        TEST_ASSERT(same16(wire, sent), "the sealed query is the vector");
        TEST_ASSERT(SpiApp_OnReceive(&app, wire) == 1u, "a telemetry query is a command that ran");
        /* VECTOR telemetry reply: 00 00 23 08 00 07 00 00 00 00 00 02 a1 01 81 7e */
        const uint8_t want[16] = {0x00, 0x00, 0x23, 0x08, 0x00, 0x07, 0, 0, 0, 0, 0, 0x02, 0xA1, 0x01, 0x81, 0x7E};
        TEST_ASSERT(same16(app.tx_wire, want), "the staged answer is the vector (latched, stop enabled, link ok, application)");
        TEST_ASSERT(app.commands_run == 1u && app.crc_errors == 0u && app.retries == 0u, "one command, nothing damaged");
    }

    /* a damaged frame is dropped and counted; a retry is not run twice; a jump of the sequence is accepted and counted */
    {
        SpiApp_t app = make_app(1u, 1u, 0u);
        Rig_t rig;
        EstopBrain_Reset(&rig.brain, 0u);
        rig.now = 0u;
        rig.expected_mask = 0u;
        rig.scan_runs = 0u;
        make_wire(wire, 0u, SPI_SYS_STOP_REQUEST, stop, 5u, 1, 1u);
        wire[5] ^= 0x01u; /* a bit of the payload flips on the wire */
        TEST_ASSERT(SpiApp_OnReceive(&app, wire) == 0u && app.crc_errors == 1u && app.trip_pending == 0u, "a damaged stop is dropped and counted");
        make_wire(wire, 0u, SPI_SYS_STOP_REQUEST, stop, 5u, 1, 2u);
        TEST_ASSERT(SpiApp_OnReceive(&app, wire) == 1u && app.trip_pending == 1u, "the intact stop is run");
        TEST_ASSERT(SpiApp_OnReceive(&app, wire) == 0u && app.retries == 1u, "the same sequence again is a retry and does not run twice");
        TEST_ASSERT(app.seq_gaps == 0u, "no gap so far (the damaged frame did not move the sequence)");
        make_wire(wire, 0u, SPI_SYS_TELEMETRY_QUERY, query, 1u, 1, 7u);
        TEST_ASSERT(SpiApp_OnReceive(&app, wire) == 1u && app.seq_gaps == 1u, "a jump of the sequence is accepted and counted");
        make_wire(wire, 0u, SPI_SYS_TELEMETRY_QUERY, query, 1u, 1, 8u);
        TEST_ASSERT(SpiApp_OnReceive(&app, wire) == 1u && app.seq_gaps == 1u, "the next sequence is no gap");
        TEST_ASSERT(app.tx_wire[5] == (SPI_SYS_FLAG_LINK_OK | SPI_SYS_FLAG_ESTOP_LATCHED | SPI_SYS_FLAG_STOP_ENABLED), "the telemetry flags");
        TEST_ASSERT(app.tx_wire[6] == 1u && app.tx_wire[7] == 1u && app.tx_wire[8] == 1u, "and it reports one CRC error, one retry and one gap");
    }

    /* a retry resends the answer staged before it instead of staging a new one */
    {
        SpiApp_t app = make_app(1u, 1u, 0u);
        make_wire(wire, 0u, SPI_SYS_TELEMETRY_QUERY, query, 1u, 1, 1u);
        (void)SpiApp_OnReceive(&app, wire);
        uint8_t first[SPI_APP_FRAME_SIZE];
        memcpy(first, app.tx_wire, sizeof first);
        (void)SpiApp_OnReceive(&app, wire);
        TEST_ASSERT(memcmp(first, app.tx_wire, sizeof first) == 0, "the answer for a retry is the same bytes with the same sequence");
    }

    /* the frames without the seal: accepted by default, refused when the build requires it */
    {
        SpiApp_t lenient = make_app(1u, 1u, 0u);
        SpiApp_t strict = make_app(1u, 1u, 1u);
        make_wire(wire, 0u, SPI_SYS_TELEMETRY_QUERY, query, 1u, 0, 0u);
        TEST_ASSERT(SpiApp_OnReceive(&lenient, wire) == 1u && lenient.unsealed == 1u, "an unsealed query is answered by default");
        TEST_ASSERT(SpiApp_OnReceive(&strict, wire) == 0u && strict.unsealed == 1u && strict.commands_run == 0u, "and refused when the seal is required");
    }

    /* what is not a command of this service is not answered: the relay frames, the update frames, the lengths and the types */
    {
        SpiApp_t app = make_app(1u, 1u, 0u);
        uint8_t keep[SPI_APP_FRAME_SIZE];
        memcpy(keep, app.tx_wire, sizeof keep);
        uint8_t seq = 0u;
        make_wire(wire, 2u, SPI_SYS_TELEMETRY_QUERY, query, 1u, 1, ++seq);
        TEST_ASSERT(SpiApp_OnReceive(&app, wire) == 0u, "a frame for the stack is not answered by the application");
        make_wire(wire, 1u, SPI_SYS_STOP_REQUEST, stop, 5u, 1, ++seq);
        TEST_ASSERT(SpiApp_OnReceive(&app, wire) == 0u && app.trip_pending == 0u, "a frame for the CM7 is not a stop");
        make_wire(wire, 0u, 0x03u, query, 1u, 1, ++seq);
        TEST_ASSERT(SpiApp_OnReceive(&app, wire) == 0u, "an update frame (a type of the CAN window) is not run by the application");
        make_wire(wire, 0u, SPI_SYS_TELEMETRY_RESPONSE, query, 1u, 1, ++seq);
        TEST_ASSERT(SpiApp_OnReceive(&app, wire) == 0u, "a response type is not a command");
        make_wire(wire, 0u, 0x27u, query, 1u, 1, ++seq);
        TEST_ASSERT(SpiApp_OnReceive(&app, wire) == 0u, "a type above the table is not a command");
        make_wire(wire, 0u, SPI_SYS_TELEMETRY_QUERY, query, 9u, 1, ++seq);
        TEST_ASSERT(SpiApp_OnReceive(&app, wire) == 0u, "a length above 8 is not a frame");
        make_wire(wire, 0u, SPI_SYS_STOP_REQUEST, stop, 4u, 1, ++seq);
        TEST_ASSERT(SpiApp_OnReceive(&app, wire) == 0u && app.trip_pending == 0u, "a stop that is too short does nothing");
        make_wire(wire, 0u, SPI_APP_IDLE_TYPE, NULL, 0u, 1, ++seq);
        TEST_ASSERT(SpiApp_OnReceive(&app, wire) == 0u, "the idle frame of the master does nothing");
        TEST_ASSERT(memcmp(keep, app.tx_wire, sizeof keep) == 0 && app.frames_ignored == 8u, "nothing was staged and eight frames were ignored");
        TEST_ASSERT(app.crc_errors == 0u && app.seq_gaps == 0u, "and none of them was damaged or out of sequence");
    }

    /* stop, release and the brain: the whole way, with the real state machine */
    {
        SpiApp_t app = make_app(1u, 1u, 0u);
        Rig_t rig;
        memset(&rig, 0, sizeof rig);
        EstopBrain_Reset(&rig.brain, 0u);
        rig.expected_mask = 0x03u;
        TEST_ASSERT(rig.brain.tripped == 1u, "the brain starts stopped");
        /* the CM5 asks for the release: the brain waits for slots 0 and 1 */
        make_wire(wire, 0u, SPI_SYS_RELEASE_REQUEST, release, 4u, 1, 1u);
        TEST_ASSERT(SpiApp_OnReceive(&app, wire) == 1u && app.release_pending == 1u, "the release request is left for the STACK A task");
        rig.now = 10u;
        rig_pump(&rig, &app);
        TEST_ASSERT(app.release_pending == 0u && rig.brain.release_state == ESTOP_RELEASE_PENDING, "the STACK A task took it once and the release started");
        rig_pump(&rig, &app);
        TEST_ASSERT(rig.brain.release_state == ESTOP_RELEASE_PENDING, "a second pass does not start it again");
        /* both boards answer READY for the epoch of this stop */
        const uint8_t ready[4] = {ESTOP_STATE_READY, rig.brain.epoch, 0u, 0u};
        EstopBrain_OnSlotState(&rig.brain, 0u, ready, 4u);
        EstopBrain_OnSlotState(&rig.brain, 1u, ready, 4u);
        uint8_t frame[8];
        (void)EstopBrain_Tick(&rig.brain, 20u, frame);
        TEST_ASSERT(rig.brain.tripped == 0u && rig.brain.release_state == ESTOP_RELEASE_DONE, "every expected board answered: the stop ended");
        rig_pump(&rig, &app);
        make_wire(wire, 0u, SPI_SYS_TELEMETRY_QUERY, query, 1u, 1, 2u);
        (void)SpiApp_OnReceive(&app, wire);
        TEST_ASSERT((app.tx_wire[5] & SPI_SYS_FLAG_ESTOP_LATCHED) == 0u, "the telemetry now says the stop is not latched");
        /* the CM5 stops the stack again, with a reason */
        make_wire(wire, 0u, SPI_SYS_STOP_REQUEST, stop, 5u, 1, 3u);
        TEST_ASSERT(SpiApp_OnReceive(&app, wire) == 1u, "the stop request ran");
        rig.now = 30u;
        rig_pump(&rig, &app);
        TEST_ASSERT(rig.brain.tripped == 1u && rig.brain.reason == 9u && rig.brain.epoch == 1u, "the brain is stopped with the reason of the frame");
        TEST_ASSERT(app.trip_pending == 0u && app.estop_latched == 1u, "the request was consumed and the state published");
        /* a stop in the middle of a release cancels it */
        make_wire(wire, 0u, SPI_SYS_RELEASE_REQUEST, release, 4u, 1, 4u);
        (void)SpiApp_OnReceive(&app, wire);
        rig.now = 40u;
        rig_pump(&rig, &app);
        TEST_ASSERT(rig.brain.release_state == ESTOP_RELEASE_PENDING, "a new release started");
        make_wire(wire, 0u, SPI_SYS_STOP_REQUEST, stop, 5u, 1, 5u);
        (void)SpiApp_OnReceive(&app, wire);
        rig.now = 50u;
        rig_pump(&rig, &app);
        TEST_ASSERT(rig.brain.release_state == ESTOP_RELEASE_NONE, "a stop from the link cancels the release in progress");
    }

    /* the probe: the request starts a scan, the answer is staged only when the scan is done */
    {
        SpiApp_t app = make_app(1u, 0u, 0u); /* the probe is read only: it needs no stop switch */
        Rig_t rig;
        memset(&rig, 0, sizeof rig);
        EstopBrain_Reset(&rig.brain, 0u);
        make_wire(wire, 0u, SPI_SYS_PROBE_REQUEST, scan, 1u, 1, 1u);
        /* VECTOR probe request: 00 00 24 01 2a 00 00 00 00 00 00 00 a1 02 b1 96 */
        const uint8_t sent[16] = {0x00, 0x00, 0x24, 0x01, 0x2A, 0, 0, 0, 0, 0, 0, 0, 0xA1, 0x02, 0xB1, 0x96};
        make_wire(wire, 0u, SPI_SYS_PROBE_REQUEST, scan, 1u, 1, 2u);
        TEST_ASSERT(same16(wire, sent), "the sealed probe request is the vector");
        make_wire(wire, 0u, SPI_SYS_PROBE_REQUEST, scan, 1u, 1, 1u);
        uint8_t before[SPI_APP_FRAME_SIZE];
        memcpy(before, app.tx_wire, sizeof before);
        TEST_ASSERT(SpiApp_OnReceive(&app, wire) == 1u && app.scan_pending == 1u && app.scan_id == 0x2Au, "the probe request leaves a scan for the STACK A task");
        SpiApp_Service(&app);
        TEST_ASSERT(memcmp(before, app.tx_wire, sizeof before) == 0, "nothing is staged while the scan runs");
        rig_pump(&rig, &app);
        TEST_ASSERT(rig.scan_runs == 1u && app.scan_pending == 0u, "the scan ran once");
        SpiApp_Service(&app);
        /* VECTOR probe reply: 00 00 25 05 2a 11 02 14 31 00 00 00 a1 01 78 2d */
        const uint8_t want[16] = {0x00, 0x00, 0x25, 0x05, 0x2A, 0x11, 0x02, 0x14, 0x31, 0, 0, 0, 0xA1, 0x01, 0x78, 0x2D};
        TEST_ASSERT(same16(app.tx_wire, want), "the answer is the probe response with the verdicts of the eight slots");
        uint8_t again[SPI_APP_FRAME_SIZE];
        memcpy(again, app.tx_wire, sizeof again);
        SpiApp_Service(&app);
        TEST_ASSERT(memcmp(again, app.tx_wire, sizeof again) == 0, "the answer is staged once, not on every pass");
    }

    /* the F-RAM request: left for the STACK A task, which owns the chip; the answer is staged by the service once it is done */
    {
        SpiApp_t app = make_app(1u, 0u, 0u);
        uint8_t request[8], taken[8], reply[8], keep[SPI_APP_FRAME_SIZE];
        FramNode_t node;
        FramNode_Init(&node, NULL);
        Fram_BuildRequest(request, FRAM_OP_ERASE, FRAM_BRAIN_ID);
        make_wire(wire, 0u, SPI_APP_FRAM_REQUEST, request, 8u, 1, 1u);
        memcpy(keep, app.tx_wire, sizeof keep);
        TEST_ASSERT(SpiApp_OnReceive(&app, wire) == 1u && app.fram_pending == 1u, "a well-formed F-RAM request is a command that ran and left a request");
        SpiApp_Service(&app);
        TEST_ASSERT(memcmp(keep, app.tx_wire, sizeof keep) == 0, "nothing is staged while the request waits");
        TEST_ASSERT(SpiApp_TakeFram(&app, taken) == 1u && memcmp(taken, request, 8) == 0 && app.fram_pending == 0u, "the STACK A task takes the 8 bytes once");
        TEST_ASSERT(SpiApp_TakeFram(&app, taken) == 0u, "and only once");
        TEST_ASSERT(FramNode_Handle(&node, FRAM_BRAIN_ID, 0u, taken, 8u, reply) == 1u && reply[2] == FRAM_STATUS_DISABLED + (HYDRA_FRAM_ERASE ? 1u : 0u), "the switch of the firmware decides the answer");
        SpiApp_FramDone(&app, reply);
        SpiApp_Service(&app);
        TEST_ASSERT(app.tx_wire[2] == SPI_APP_FRAM_RESPONSE && app.tx_wire[3] == 8u && memcmp(&app.tx_wire[4], reply, 8) == 0, "the answer is the F-RAM response of 8 bytes");
        memcpy(keep, app.tx_wire, sizeof keep);
        SpiApp_Service(&app);
        TEST_ASSERT(memcmp(keep, app.tx_wire, sizeof keep) == 0, "the answer is staged once, not on every pass");
        make_wire(wire, 0u, SPI_APP_FRAM_REQUEST, request, 7u, 1, 2u);
        TEST_ASSERT(SpiApp_OnReceive(&app, wire) == 0u && app.fram_pending == 0u, "a short request leaves nothing");
        make_wire(wire, 1u, SPI_APP_FRAM_REQUEST, request, 8u, 1, 3u);
        TEST_ASSERT(SpiApp_OnReceive(&app, wire) == 0u && app.fram_pending == 0u, "a request for another tier is not ours: it goes to the stack, never to the chip");
        make_wire(wire, 0u, SPI_APP_FRAM_REQUEST, request, 8u, 1, 4u);
        (void)SpiApp_OnReceive(&app, wire);
        make_wire(wire, 0u, SPI_APP_FRAM_REQUEST, request, 8u, 1, 5u);
        TEST_ASSERT(SpiApp_OnReceive(&app, wire) == 1u && app.fram_done == 0u && app.fram_pending == 1u, "a new request clears a stale answer");
    }

    /* read only: stop and release are refused and leave no request */
    {
        SpiApp_t app = make_app(1u, 0u, 0u);
        make_wire(wire, 0u, SPI_SYS_STOP_REQUEST, stop, 5u, 1, 1u);
        TEST_ASSERT(SpiApp_OnReceive(&app, wire) == 0u && app.trip_pending == 0u, "read only: a stop leaves no request");
        make_wire(wire, 0u, SPI_SYS_RELEASE_REQUEST, release, 4u, 1, 2u);
        TEST_ASSERT(SpiApp_OnReceive(&app, wire) == 0u && app.release_pending == 0u, "read only: a release leaves no request");
        make_wire(wire, 0u, SPI_SYS_TELEMETRY_QUERY, query, 1u, 1, 3u);
        TEST_ASSERT(SpiApp_OnReceive(&app, wire) == 1u && (app.tx_wire[5] & SPI_SYS_FLAG_STOP_ENABLED) == 0u, "read only: the telemetry says the stop commands are off");
    }

    /* the heartbeat of the safety-zones chain: counted by the SPI task, timed by the STACK A task, never before the first one */
    {
        SpiApp_t app = make_app(1u, 1u, 0u);
        uint8_t keep[SPI_APP_FRAME_SIZE];
        memcpy(keep, app.tx_wire, sizeof keep);
        TEST_ASSERT(SpiApp_ZonesTick(&app, 5000u, 1000u) == 0u, "no heartbeat has come: the watchdog is not armed and never trips");
        TEST_ASSERT(SpiApp_ZonesTick(&app, 90000u, 1000u) == 0u, "however long it takes");
        make_wire(wire, 0u, SPI_SYS_ZONES_HEARTBEAT, zone, 4u, 1, 1u);
        TEST_ASSERT(SpiApp_OnReceive(&app, wire) == 1u && app.zones_beat == 1u, "a heartbeat is a command that ran and was counted");
        TEST_ASSERT(memcmp(keep, app.tx_wire, sizeof keep) == 0, "it is not answered");
        TEST_ASSERT(SpiApp_ZonesTick(&app, 100000u, 1000u) == 0u && app.zones_armed == 1u, "the STACK A task notes the time and arms the watchdog");
        TEST_ASSERT(SpiApp_ZonesTick(&app, 101000u, 1000u) == 0u, "exactly the maximum age still counts");
        TEST_ASSERT(SpiApp_ZonesTick(&app, 101001u, 1000u) == 1u, "one millisecond more: the heartbeat was lost");
        TEST_ASSERT(SpiApp_ZonesTick(&app, 102000u, 1000u) == 0u && app.zones_armed == 0u, "a loss is reported once");
        make_wire(wire, 0u, SPI_SYS_ZONES_HEARTBEAT, zone, 4u, 1, 2u);
        (void)SpiApp_OnReceive(&app, wire);
        TEST_ASSERT(SpiApp_ZonesTick(&app, 103000u, 1000u) == 0u && app.zones_armed == 1u, "the next heartbeat arms it again");
        TEST_ASSERT(SpiApp_ZonesTick(&app, 104001u, 1000u) == 1u, "and the next loss is reported");
        /* the clock wraps */
        make_wire(wire, 0u, SPI_SYS_ZONES_HEARTBEAT, zone, 4u, 1, 3u);
        (void)SpiApp_OnReceive(&app, wire);
        TEST_ASSERT(SpiApp_ZonesTick(&app, 0xFFFFFF00u, 1000u) == 0u, "heartbeat noted just before the wrap");
        TEST_ASSERT(SpiApp_ZonesTick(&app, 0x00000100u, 1000u) == 0u, "512 ms later across the wrap: still healthy");
        TEST_ASSERT(SpiApp_ZonesTick(&app, 0x00000500u, 1000u) == 1u, "1.5 s later across the wrap: lost");
        /* malformed heartbeats do nothing */
        const uint8_t wrong[4] = {'Z', 'O', 'N', 'X'};
        make_wire(wire, 0u, SPI_SYS_ZONES_HEARTBEAT, wrong, 4u, 1, 4u);
        TEST_ASSERT(SpiApp_OnReceive(&app, wire) == 0u, "a heartbeat with a wrong magic does nothing");
        make_wire(wire, 0u, SPI_SYS_ZONES_HEARTBEAT, zone, 3u, 1, 5u);
        TEST_ASSERT(SpiApp_OnReceive(&app, wire) == 0u, "a short heartbeat does nothing");
        TEST_ASSERT(app.zones_beat == 3u, "and neither was counted");
        /* against the real brain: running, the lost heartbeat stops it with the reason of the safety zones */
        Rig_t rig;
        memset(&rig, 0, sizeof rig);
        EstopBrain_Reset(&rig.brain, 0u);
        (void)EstopBrain_RequestRelease(&rig.brain, 1u, 0u);
        uint8_t frame[8];
        (void)EstopBrain_Tick(&rig.brain, 2u, frame);
        TEST_ASSERT(rig.brain.tripped == 0u, "the brain runs");
        make_wire(wire, 0u, SPI_SYS_ZONES_HEARTBEAT, zone, 4u, 1, 6u);
        (void)SpiApp_OnReceive(&app, wire);
        (void)SpiApp_ZonesTick(&app, 1000u, 1000u);
        if (SpiApp_ZonesTick(&app, 2001u, 1000u)) EstopBrain_Trip(&rig.brain, 2001u, SPI_APP_ZONES_REASON);
        TEST_ASSERT(rig.brain.tripped == 1u && rig.brain.reason == ESTOP_REASON_SAFETY_ZONE && SPI_APP_ZONES_REASON == ESTOP_REASON_SAFETY_ZONE,
                    "the stack stopped with the reason safety zone");
    }

    /* read only: the heartbeat is refused because acting on the stack needs the stop switch */
    {
        SpiApp_t app = make_app(1u, 0u, 0u);
        make_wire(wire, 0u, SPI_SYS_ZONES_HEARTBEAT, zone, 4u, 1, 1u);
        TEST_ASSERT(SpiApp_OnReceive(&app, wire) == 0u && app.zones_beat == 0u, "read only: a heartbeat is not counted");
    }

    /* the sequence of the frames the service sends wraps around without breaking the seal */
    {
        SpiApp_t app = make_app(1u, 1u, 0u);
        uint8_t last = 0u;
        for (unsigned i = 0; i < 300u; i++) {
            make_wire(wire, 0u, SPI_SYS_TELEMETRY_QUERY, query, 1u, 1, (uint8_t)(i + 1u));
            (void)SpiApp_OnReceive(&app, wire);
            uint8_t seq = 0u;
            if (SpiWire_Check(app.tx_wire, &seq) != SPI_WIRE_SEALED_OK) {
                TEST_ASSERT(0, "every staged answer is sealed");
                break;
            }
            if (i > 0u && (uint8_t)(seq - last) != 1u) {
                TEST_ASSERT(0, "the sequence of the answers moves by one");
                break;
            }
            last = seq;
        }
        TEST_ASSERT(app.seq_gaps == 0u && app.crc_errors == 0u && app.commands_run == 300u, "300 queries across the wrap of the sequence: no gap, no error");
    }
}

static void run_with_switches_off(void)
{
    uint8_t wire[SPI_APP_FRAME_SIZE];
    const uint8_t query[1] = {0u};
    const uint8_t stop[5] = {'S', 'T', 'O', 'P', 9u};
    const uint8_t release[4] = {'R', 'E', 'L', 'S'};
    const uint8_t scan[1] = {0x2Au};
    const uint8_t zone[4] = {'Z', 'O', 'N', 'E'};
    const SpiSysConfig_t build = SpiSys_ConfigFromBuild();
    TEST_ASSERT(build.telemetry_enabled == 0u && build.stop_enabled == 0u, "the build as it ships has both switches off");
    TEST_ASSERT(HYDRA_SPI_APP == 0 && HYDRA_SPI_REQUIRE_CRC == 0, "and the service and the seal requirement are off");

    SpiApp_t app = make_app(build.telemetry_enabled, build.stop_enabled, 0u);
    uint8_t keep[SPI_APP_FRAME_SIZE];
    memcpy(keep, app.tx_wire, sizeof keep);
    uint8_t seq = 0u;
    make_wire(wire, 0u, SPI_SYS_TELEMETRY_QUERY, query, 1u, 1, ++seq);
    TEST_ASSERT(SpiApp_OnReceive(&app, wire) == 0u, "off: telemetry is not answered");
    make_wire(wire, 0u, SPI_SYS_STOP_REQUEST, stop, 5u, 1, ++seq);
    TEST_ASSERT(SpiApp_OnReceive(&app, wire) == 0u && app.trip_pending == 0u, "off: a stop leaves no request");
    make_wire(wire, 0u, SPI_SYS_RELEASE_REQUEST, release, 4u, 1, ++seq);
    TEST_ASSERT(SpiApp_OnReceive(&app, wire) == 0u && app.release_pending == 0u, "off: a release leaves no request");
    make_wire(wire, 0u, SPI_SYS_PROBE_REQUEST, scan, 1u, 1, ++seq);
    TEST_ASSERT(SpiApp_OnReceive(&app, wire) == 0u && app.scan_pending == 0u, "off: a probe starts no scan");
    make_wire(wire, 0u, SPI_SYS_ZONES_HEARTBEAT, zone, 4u, 1, ++seq);
    TEST_ASSERT(SpiApp_OnReceive(&app, wire) == 0u && app.zones_beat == 0u, "off: a heartbeat is not counted");
    {
        uint8_t fram[8];
        Fram_BuildRequest(fram, FRAM_OP_ERASE, FRAM_BRAIN_ID);
        make_wire(wire, 0u, SPI_APP_FRAM_REQUEST, fram, 8u, 1, ++seq);
        TEST_ASSERT(SpiApp_OnReceive(&app, wire) == 0u && app.fram_pending == 0u, "off: an F-RAM request leaves no request");
    }
    TEST_ASSERT(memcmp(keep, app.tx_wire, sizeof keep) == 0 && app.commands_run == 0u && app.frames_ignored == 6u, "off: nothing staged, nothing run, six ignored");
    SpiApp_Service(&app);
    TEST_ASSERT(memcmp(keep, app.tx_wire, sizeof keep) == 0, "off: the service stages nothing between transactions");
}

int main(void)
{
    printf("HYDRA-UMC host logic tests - spi_app.h (switches %d)\n", SPI_APP_TEST_SWITCHES);
    if (SPI_APP_TEST_SWITCHES) run_with_switches_on();
    else run_with_switches_off();
    if (failures) printf("%d FAILED\n", failures);
    else printf("ok\n");
    return failures ? 1 : 0;
}
