/* =============================================================================
 * HYDRA-UMC - src/mcu_stm32h745/CM4/spi_system.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Commands of the SPI link that the CM4 answers itself and never puts on a CAN bus (HAL-free, host-testable). docs/SPI_SYSTEM.md is the contract and
 * tools/spi_system.py the reference in Python. All of them go to the tier SELF of the frame; the frame types start at 0x20, above the window of 32
 * identifiers a slot owns, so none of them can be mistaken for a CAN message.
 *
 *   0x20 STOP_REQUEST       dlc 5   'S' 'T' 'O' 'P' reason     the CM5 asks the brain to put the whole stack in the emergency stop
 *   0x21 RELEASE_REQUEST    dlc 4   'R' 'E' 'L' 'S'            a person asked to end the stop; the brain still waits for every board to say READY
 *   0x22 TELEMETRY_QUERY    dlc 1   selector (0 = this core)
 *   0x23 TELEMETRY_RESPONSE dlc 8   selector, flags, crc errors, retries, sequence gaps, CAN tx errors, CAN rx errors, state (counters saturate at 255)
 *   0x24 PROBE_REQUEST      dlc 1   scan id: scan the slots before an update
 *   0x25 PROBE_RESPONSE     dlc 5   scan id, then the verdict of the slots 0 to 7 in a nibble each
 *
 * Two switches, both 0 by default, so the firmware answers none of these until a build asks for them:
 *   HYDRA_SPI_SYSTEM        telemetry and probe (read only)
 *   HYDRA_SPI_STOP_COMMAND  stop and release (act on the stack; needs HYDRA_SPI_SYSTEM too)
 * The functions take the switches in a configuration structure, so the host test runs every combination in one binary.
 * ========================================================================== */
#ifndef HYDRA_UMC_SPI_SYSTEM_H
#define HYDRA_UMC_SPI_SYSTEM_H

#include <stdint.h>
#include <string.h>

#ifndef HYDRA_SPI_SYSTEM
#define HYDRA_SPI_SYSTEM 0
#endif
#ifndef HYDRA_SPI_STOP_COMMAND
#define HYDRA_SPI_STOP_COMMAND 0
#endif

#define SPI_SYS_STOP_REQUEST       0x20u
#define SPI_SYS_RELEASE_REQUEST    0x21u
#define SPI_SYS_TELEMETRY_QUERY    0x22u
#define SPI_SYS_TELEMETRY_RESPONSE 0x23u
#define SPI_SYS_PROBE_REQUEST      0x24u
#define SPI_SYS_PROBE_RESPONSE     0x25u

#define SPI_SYS_STATE_BOOTLOADER   0x01u
#define SPI_SYS_STATE_APPLICATION  0x02u
#define SPI_SYS_FLAG_LINK_OK       0x01u
#define SPI_SYS_FLAG_ESTOP_LATCHED 0x02u
#define SPI_SYS_FLAG_STOP_ENABLED  0x04u

typedef struct {
    uint8_t telemetry_enabled; /* HYDRA_SPI_SYSTEM */
    uint8_t stop_enabled;      /* HYDRA_SPI_STOP_COMMAND */
} SpiSysConfig_t;

typedef struct {
    uint32_t crc_errors;
    uint32_t retries;
    uint32_t seq_gaps;
    uint8_t can_tx_errors;
    uint8_t can_rx_errors;
    uint8_t state;       /* SPI_SYS_STATE_* */
    uint8_t link_ok;
    uint8_t estop_latched;
    uint8_t scan_verdicts[8]; /* the last slot scan: 0 absent, 1 ok, 2 duplicate, 3 misplaced, 4 noisy (tools/slot_scan.py) */
    uint8_t scan_id;
} SpiSysSnapshot_t;

typedef enum {
    SPI_SYS_ACT_NONE = 0,         /* not a command of this file, a switch is off, or the frame is malformed: nothing is sent back */
    SPI_SYS_ACT_REPLY,            /* send `out` back to the CM5 */
    SPI_SYS_ACT_TRIP,             /* the caller puts the stack in the emergency stop with the reason in out[0] */
    SPI_SYS_ACT_REQUEST_RELEASE,  /* the caller starts the release procedure of the emergency stop (EstopBrain_RequestRelease) */
    SPI_SYS_ACT_START_SCAN        /* the caller runs the slot scan with the id in out[0] and answers with the verdicts */
} SpiSysAction;

static inline SpiSysConfig_t SpiSys_ConfigFromBuild(void)
{
    SpiSysConfig_t config = {(uint8_t)HYDRA_SPI_SYSTEM, (uint8_t)HYDRA_SPI_STOP_COMMAND};
    return config;
}

static inline uint8_t SpiSys_Saturate(uint32_t value)
{
    return (uint8_t)(value > 255u ? 255u : value);
}

static inline uint8_t SpiSys_IsCommand(uint8_t frame_type)
{
    return (uint8_t)(frame_type >= SPI_SYS_STOP_REQUEST && frame_type <= SPI_SYS_PROBE_REQUEST && frame_type != SPI_SYS_TELEMETRY_RESPONSE);
}

/* Fills the telemetry answer (8 bytes). */
static inline void SpiSys_BuildTelemetry(const SpiSysConfig_t *config, const SpiSysSnapshot_t *snap, uint8_t selector, uint8_t out[8])
{
    out[0] = selector;
    out[1] = (uint8_t)((snap->link_ok ? SPI_SYS_FLAG_LINK_OK : 0u) | (snap->estop_latched ? SPI_SYS_FLAG_ESTOP_LATCHED : 0u) |
                       (config->stop_enabled ? SPI_SYS_FLAG_STOP_ENABLED : 0u));
    out[2] = SpiSys_Saturate(snap->crc_errors);
    out[3] = SpiSys_Saturate(snap->retries);
    out[4] = SpiSys_Saturate(snap->seq_gaps);
    out[5] = snap->can_tx_errors;
    out[6] = snap->can_rx_errors;
    out[7] = snap->state;
}

static inline void SpiSys_BuildProbeResponse(const SpiSysSnapshot_t *snap, uint8_t scan_id, uint8_t out[8])
{
    memset(out, 0, 8);
    out[0] = scan_id;
    for (int slot = 0; slot < 8; slot++) {
        const uint8_t nibble = (uint8_t)(snap->scan_verdicts[slot] & 0x0Fu);
        out[1 + slot / 2] = (uint8_t)(out[1 + slot / 2] | (slot % 2 == 0 ? nibble : (uint8_t)(nibble << 4)));
    }
}

/* One frame of the tier SELF: what to do and, for a reply, the bytes. `out_dlc` is the length of `out` for a reply. Anything that is not exactly
 * a command of the table above (wrong length, wrong magic, unknown selector) does nothing: a frame never half-works. */
static inline SpiSysAction SpiSys_Handle(const SpiSysConfig_t *config, const SpiSysSnapshot_t *snap, uint8_t frame_type, const uint8_t *payload,
                                         uint8_t dlc, uint8_t *out_type, uint8_t out[8], uint8_t *out_dlc)
{
    memset(out, 0, 8);
    *out_type = 0u;
    *out_dlc = 0u;
    if (!config->telemetry_enabled) return SPI_SYS_ACT_NONE;
    switch (frame_type) {
    case SPI_SYS_TELEMETRY_QUERY:
        if (dlc != 1u || payload[0] != 0u) return SPI_SYS_ACT_NONE;
        SpiSys_BuildTelemetry(config, snap, payload[0], out);
        *out_type = SPI_SYS_TELEMETRY_RESPONSE;
        *out_dlc = 8u;
        return SPI_SYS_ACT_REPLY;
    case SPI_SYS_PROBE_REQUEST:
        if (dlc != 1u) return SPI_SYS_ACT_NONE;
        out[0] = payload[0];
        return SPI_SYS_ACT_START_SCAN;
    case SPI_SYS_STOP_REQUEST:
        if (!config->stop_enabled || dlc != 5u || payload[0] != 'S' || payload[1] != 'T' || payload[2] != 'O' || payload[3] != 'P') return SPI_SYS_ACT_NONE;
        out[0] = payload[4];
        return SPI_SYS_ACT_TRIP;
    case SPI_SYS_RELEASE_REQUEST:
        if (!config->stop_enabled || dlc != 4u || payload[0] != 'R' || payload[1] != 'E' || payload[2] != 'L' || payload[3] != 'S') return SPI_SYS_ACT_NONE;
        return SPI_SYS_ACT_REQUEST_RELEASE;
    default:
        return SPI_SYS_ACT_NONE;
    }
}

/* The answer to a probe once the scan is done. */
static inline void SpiSys_ProbeAnswer(const SpiSysSnapshot_t *snap, uint8_t scan_id, uint8_t *out_type, uint8_t out[8], uint8_t *out_dlc)
{
    SpiSys_BuildProbeResponse(snap, scan_id, out);
    *out_type = SPI_SYS_PROBE_RESPONSE;
    *out_dlc = 5u;
}

#endif /* HYDRA_UMC_SPI_SYSTEM_H */
