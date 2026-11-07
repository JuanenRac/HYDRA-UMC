/* =============================================================================
 * HYDRA-UMC Firmware - host logic tests for spi_system.h (stop, release, telemetry and probe over the SPI link)
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Built against src/mcu_stm32h745/CM4 so the stop and release actions run the real state machine of the brain (estop_brain.h). The bytes of the
 * vectors are the ones tools/spi_system.py builds (tests/test_spi_system.py compares them). Evidence: simulator; the link at full rate is bench
 * row 13 and the telemetry over ten minutes is row 19 of docs/BENCH_TESTS.md.
 */
#include <stdio.h>
#include <string.h>

#include "estop_brain.h"
#include "spi_system.h"

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("  FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
            failures++; \
        } \
    } while (0)

static int failures;

static SpiSysSnapshot_t snapshot(void)
{
    SpiSysSnapshot_t snap;
    memset(&snap, 0, sizeof snap);
    snap.crc_errors = 3u;
    snap.retries = 300u;       /* saturates */
    snap.seq_gaps = 0u;
    snap.can_tx_errors = 5u;
    snap.can_rx_errors = 9u;
    snap.state = SPI_SYS_STATE_APPLICATION;
    snap.link_ok = 1u;
    snap.estop_latched = 1u;
    const uint8_t verdicts[8] = {1u, 1u, 2u, 0u, 4u, 1u, 1u, 3u};
    memcpy(snap.scan_verdicts, verdicts, 8);
    return snap;
}

static SpiSysAction run(const SpiSysConfig_t *config, uint8_t type, const uint8_t *payload, uint8_t dlc, uint8_t *out_type, uint8_t out[8], uint8_t *out_dlc)
{
    const SpiSysSnapshot_t snap = snapshot();
    return SpiSys_Handle(config, &snap, type, payload, dlc, out_type, out, out_dlc);
}

int main(void)
{
    printf("HYDRA-UMC host logic tests - spi_system.h\n");
    uint8_t out_type, out[8], out_dlc;
    const SpiSysConfig_t off = {0u, 0u};
    const SpiSysConfig_t read_only = {1u, 0u};
    const SpiSysConfig_t full = {1u, 1u};
    const uint8_t query[1] = {0u};
    const uint8_t stop[5] = {'S', 'T', 'O', 'P', 7u};
    const uint8_t release[4] = {'R', 'E', 'L', 'S'};
    const uint8_t scan[1] = {0x2Au};

    TEST_ASSERT(SpiSys_ConfigFromBuild().telemetry_enabled == HYDRA_SPI_SYSTEM && SpiSys_ConfigFromBuild().stop_enabled == HYDRA_SPI_STOP_COMMAND,
                "the configuration of the build is the two switches");

    /* every switch off: not one of the commands does anything, and nothing is sent back */
    TEST_ASSERT(run(&off, SPI_SYS_TELEMETRY_QUERY, query, 1u, &out_type, out, &out_dlc) == SPI_SYS_ACT_NONE && out_dlc == 0u, "off: telemetry ignored");
    TEST_ASSERT(run(&off, SPI_SYS_STOP_REQUEST, stop, 5u, &out_type, out, &out_dlc) == SPI_SYS_ACT_NONE, "off: stop ignored");
    TEST_ASSERT(run(&off, SPI_SYS_RELEASE_REQUEST, release, 4u, &out_type, out, &out_dlc) == SPI_SYS_ACT_NONE, "off: release ignored");
    TEST_ASSERT(run(&off, SPI_SYS_PROBE_REQUEST, scan, 1u, &out_type, out, &out_dlc) == SPI_SYS_ACT_NONE, "off: probe ignored");

    /* read only: telemetry and probe answer, stop and release do not */
    TEST_ASSERT(run(&read_only, SPI_SYS_TELEMETRY_QUERY, query, 1u, &out_type, out, &out_dlc) == SPI_SYS_ACT_REPLY, "read only: telemetry answers");
    TEST_ASSERT(out_type == SPI_SYS_TELEMETRY_RESPONSE && out_dlc == 8u, "the answer is the telemetry response of 8 bytes");
    /* VECTOR telemetry (stop disabled): 00 03 03 ff 00 05 09 02 */
    {
        const uint8_t want[8] = {0x00u, 0x03u, 0x03u, 0xFFu, 0x00u, 0x05u, 0x09u, 0x02u};
        TEST_ASSERT(memcmp(out, want, 8) == 0, "telemetry bytes, stop command disabled");
    }
    TEST_ASSERT(run(&read_only, SPI_SYS_STOP_REQUEST, stop, 5u, &out_type, out, &out_dlc) == SPI_SYS_ACT_NONE, "read only: stop ignored");
    TEST_ASSERT(run(&read_only, SPI_SYS_RELEASE_REQUEST, release, 4u, &out_type, out, &out_dlc) == SPI_SYS_ACT_NONE, "read only: release ignored");
    TEST_ASSERT(run(&read_only, SPI_SYS_PROBE_REQUEST, scan, 1u, &out_type, out, &out_dlc) == SPI_SYS_ACT_START_SCAN && out[0] == 0x2Au, "read only: probe starts a scan");

    /* both on */
    TEST_ASSERT(run(&full, SPI_SYS_TELEMETRY_QUERY, query, 1u, &out_type, out, &out_dlc) == SPI_SYS_ACT_REPLY, "full: telemetry answers");
    /* VECTOR telemetry (stop enabled): 00 07 03 ff 00 05 09 02 */
    {
        const uint8_t want[8] = {0x00u, 0x07u, 0x03u, 0xFFu, 0x00u, 0x05u, 0x09u, 0x02u};
        TEST_ASSERT(memcmp(out, want, 8) == 0, "telemetry bytes, stop command enabled");
    }
    TEST_ASSERT(run(&full, SPI_SYS_STOP_REQUEST, stop, 5u, &out_type, out, &out_dlc) == SPI_SYS_ACT_TRIP && out[0] == 7u, "full: stop asks for a trip with the reason");
    TEST_ASSERT(run(&full, SPI_SYS_RELEASE_REQUEST, release, 4u, &out_type, out, &out_dlc) == SPI_SYS_ACT_REQUEST_RELEASE, "full: release asks for the procedure");

    /* the heartbeat of the safety-zones chain: it acts on the stack, so it needs the stop switch, and the exact magic */
    {
        const uint8_t zone[4] = {'Z', 'O', 'N', 'E'};
        const uint8_t wrong[4] = {'Z', 'O', 'N', 'X'};
        TEST_ASSERT(SpiSys_IsCommand(SPI_SYS_ZONES_HEARTBEAT) && !SpiSys_IsCommand(0x27u), "the heartbeat is a command, the type above it is not");
        TEST_ASSERT(run(&full, SPI_SYS_ZONES_HEARTBEAT, zone, 4u, &out_type, out, &out_dlc) == SPI_SYS_ACT_ZONES_HEARTBEAT && out_dlc == 0u, "full: the heartbeat is noted and not answered");
        TEST_ASSERT(run(&read_only, SPI_SYS_ZONES_HEARTBEAT, zone, 4u, &out_type, out, &out_dlc) == SPI_SYS_ACT_NONE, "read only: the heartbeat is ignored");
        TEST_ASSERT(run(&off, SPI_SYS_ZONES_HEARTBEAT, zone, 4u, &out_type, out, &out_dlc) == SPI_SYS_ACT_NONE, "off: the heartbeat is ignored");
        TEST_ASSERT(run(&full, SPI_SYS_ZONES_HEARTBEAT, wrong, 4u, &out_type, out, &out_dlc) == SPI_SYS_ACT_NONE, "a wrong magic does nothing");
        TEST_ASSERT(run(&full, SPI_SYS_ZONES_HEARTBEAT, zone, 3u, &out_type, out, &out_dlc) == SPI_SYS_ACT_NONE, "a short heartbeat does nothing");
        TEST_ASSERT(run(&full, SPI_SYS_ZONES_HEARTBEAT, zone, 5u, &out_type, out, &out_dlc) == SPI_SYS_ACT_NONE, "a long heartbeat does nothing");
    }

    /* the probe answer: the verdicts of the eight slots in nibbles */
    {
        const SpiSysSnapshot_t snap = snapshot();
        SpiSys_ProbeAnswer(&snap, 0x2Au, &out_type, out, &out_dlc);
        /* VECTOR probe response: 2a 11 02 14 31 */
        const uint8_t want[5] = {0x2Au, 0x11u, 0x02u, 0x14u, 0x31u};
        TEST_ASSERT(out_type == SPI_SYS_PROBE_RESPONSE && out_dlc == 5u && memcmp(out, want, 5) == 0 && out[5] == 0u, "probe response bytes");
    }

    /* malformed frames do nothing, with every switch on */
    {
        const uint8_t bad_magic[5] = {'S', 'T', 'O', 'Q', 7u};
        const uint8_t bad_selector[1] = {1u};
        TEST_ASSERT(run(&full, SPI_SYS_STOP_REQUEST, bad_magic, 5u, &out_type, out, &out_dlc) == SPI_SYS_ACT_NONE, "a stop with a wrong magic does nothing");
        TEST_ASSERT(run(&full, SPI_SYS_STOP_REQUEST, stop, 4u, &out_type, out, &out_dlc) == SPI_SYS_ACT_NONE, "a stop that is too short does nothing");
        TEST_ASSERT(run(&full, SPI_SYS_STOP_REQUEST, stop, 6u, &out_type, out, &out_dlc) == SPI_SYS_ACT_NONE, "a stop that is too long does nothing");
        TEST_ASSERT(run(&full, SPI_SYS_RELEASE_REQUEST, release, 3u, &out_type, out, &out_dlc) == SPI_SYS_ACT_NONE, "a short release does nothing");
        TEST_ASSERT(run(&full, SPI_SYS_TELEMETRY_QUERY, bad_selector, 1u, &out_type, out, &out_dlc) == SPI_SYS_ACT_NONE, "an unknown selector does nothing");
        TEST_ASSERT(run(&full, SPI_SYS_TELEMETRY_QUERY, query, 0u, &out_type, out, &out_dlc) == SPI_SYS_ACT_NONE, "a query without payload does nothing");
        TEST_ASSERT(run(&full, SPI_SYS_TELEMETRY_RESPONSE, query, 1u, &out_type, out, &out_dlc) == SPI_SYS_ACT_NONE, "a response type is not a command");
        TEST_ASSERT(run(&full, 0x1Fu, query, 1u, &out_type, out, &out_dlc) == SPI_SYS_ACT_NONE, "a type of the CAN window is not a command of this file");
        TEST_ASSERT(!SpiSys_IsCommand(0x1Fu) && !SpiSys_IsCommand(SPI_SYS_TELEMETRY_RESPONSE) && !SpiSys_IsCommand(SPI_SYS_PROBE_RESPONSE) && SpiSys_IsCommand(SPI_SYS_STOP_REQUEST),
                    "which types are commands");
    }

    /* the actions against the real brain */
    {
        EstopBrain_t brain;
        EstopBrain_Reset(&brain, 0u);
        TEST_ASSERT(brain.tripped == 1u, "the brain starts stopped");
        TEST_ASSERT(EstopBrain_RequestRelease(&brain, 10u, 0x03u) == 1u, "the release request starts the procedure from the stopped state");
        TEST_ASSERT(brain.release_state == ESTOP_RELEASE_PENDING, "and it waits for the boards");
        EstopBrain_Trip(&brain, 20u, 7u);   /* a trip from the SPI link cancels the release in progress */
        TEST_ASSERT(brain.release_state == ESTOP_RELEASE_NONE && brain.reason == 7u, "a trip with the reason of the frame cancels it");
        TEST_ASSERT(EstopBrain_RequestRelease(&brain, 30u, 0x03u) == 1u, "a new release can start afterwards");
        TEST_ASSERT(EstopBrain_RequestRelease(&brain, 31u, 0x03u) == 0u, "but not twice at once");
    }

    if (failures) printf("%d FAILED\n", failures);
    else printf("ok\n");
    return failures ? 1 : 0;
}
