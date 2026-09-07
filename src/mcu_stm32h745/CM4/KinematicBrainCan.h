/*
 * =============================================================================
 * KinematicBrainCan.h - Real FDCAN1 "STACK A" master + Tier 2/3 relay tunnel
 * PROJECT: HYDRA-UMC (Kinematic Brain firmware, Tier 0 - STM32H745ZIT6)
 * AUTHOR: JuanenRac (Electro Hobby 3D) - electrohobby3d@gmail.com
 * LICENSE: GPL-3.0 - see repo root LICENSE
 *
 * This core's real application-level job per README.md section 5 and
 * docs/architecture.md sections 4-5: talk to the up-to-8 Robot Controller
 * Boards on STACK A, and tunnel opaque CAN traffic one hop further to each
 * board's own URTC Tool Head (and, through that, its optional Advanced
 * Expansion Board) via a real RELAY_SEND/RELAY_RECV tunnel.
 *
 * The fragmentation scheme for RELAY_SEND/RELAY_RECV below is this
 * project's own real, explicit design (also implemented, independently
 * but to the exact same spec, on the CM5 host side - see
 * src/cm5_host/spi_bridge/spi_bridge/relay_tunnel.py's own docstring for
 * the full reasoning): each RELAY_SEND/RELAY_RECV frame carries a real
 * target/source CAN ID (2 bytes BE) + real total DLC (1 byte) + up to 5
 * real data bytes; an 8-byte CAN frame needs exactly 2 fragments.
 *
 * Deliberately does NOT compute trajectory, gait or any real-time motion
 * itself - only forwards named AXIS_STATUS/AXIS_STEP_TELEMETRY queries and
 * opaque relay traffic. The target Robot Controller Board's own firmware
 * keeps full authority over real motion.
 */
#ifndef KINEMATIC_BRAIN_CAN_H
#define KINEMATIC_BRAIN_CAN_H

#include <stdint.h>

/* CAN_ID_STACKA_BASE/STACKA_SLOT_WINDOW/OFS_RELAY_SEND/OFS_RELAY_RECV are
 * already defined in bootloader_common.h (this application reuses that
 * same header for the shared constants - see .c file's own includes). */

/* Real, explicit fragmentation-header size for one RELAY_SEND/RELAY_RECV
 * payload (2-byte CAN ID + 1-byte total DLC), leaving 5 real data bytes
 * inside the real 8-byte payload[] field every tier's own frame format
 * already uses (SpiOtaFrame_t on SPI1, IpcFrame_t on the mailbox, and the
 * plain 8-byte FDCAN payload here). */
#define RELAY_FRAGMENT_HEADER_BYTES 3
#define RELAY_FRAGMENT_DATA_BYTES   5

/* Real STM32H7 FDCAN1 init, reusing the exact same real config the
 * bootloader's own MX_FDCAN1_Init() already proved (Classic CAN, ~1 Mbps
 * @ HSI64-derived kernel clock - re-verify once this core's own real
 * clock tree change above lands, since the FDCAN kernel clock source
 * this prescaler assumes may shift with it). Call once from main(). */
void KinematicBrainCan_Init(void);

/* Real slot-addressed AXIS_STATUS query (+0x10) - sends a real Classic CAN
 * frame to CAN_ID_STACKA_BASE + slot*STACKA_SLOT_WINDOW + 0x10 and waits
 * (bounded, non-blocking-forever) for a real response. Returns 1 and
 * fills out_status/out_dlc on success, 0 on timeout/no response - a
 * timeout is real, expected behavior for an unpopulated slot, not an
 * error condition to alarm on by itself. */
uint8_t KinematicBrainCan_QueryAxisStatus(uint8_t slot, uint8_t out_status[8], uint8_t *out_dlc, uint32_t timeout_ms);

/* Real, tunneled request/response through a real RELAY_SEND/RELAY_RECV
 * round trip to Tier 2 (the URTC Tool Head on the given slot's own second
 * CAN controller) - `real_can_id` is a real URTC bootloader/telemetry ID
 * (e.g. 0x7F0-0x7FF, docs/CANBUS.TXT in the sibling URTC repo), `data`/
 * `dlc` the real outbound payload. Returns 1 and fills out_data/out_dlc
 * on success, 0 on timeout - fire-and-forget frame types (matching the
 * CM5 host side's own _QUERY_FRAME_TYPES reasoning) should pass
 * `wait_for_response = 0` to skip the wait entirely. */
uint8_t KinematicBrainCan_RelayToUrtcHead(
    uint8_t slot,
    uint16_t real_can_id,
    const uint8_t *data,
    uint8_t dlc,
    uint8_t wait_for_response,
    uint8_t out_data[8],
    uint8_t *out_dlc,
    uint32_t timeout_ms
);

#endif /* KINEMATIC_BRAIN_CAN_H */
