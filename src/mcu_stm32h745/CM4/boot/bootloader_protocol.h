// =============================================================================
// HYDRA-UMC Kinematic Brain Bootloader (CM4) - self-update state machine,
// application-jump, and 3-way relay declarations
// Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
// GPL-3.0 - see repo root LICENSE
// =============================================================================
#ifndef BOOTLOADER_PROTOCOL_H
#define BOOTLOADER_PROTOCOL_H

#include <stdint.h>
#include "bootloader_common.h"

extern uint32_t update_total_size;
extern uint32_t update_bytes_received;
extern uint8_t  update_in_progress;
extern uint32_t update_last_activity_tick;
extern uint8_t  update_failed;

// Unlike every other bootloader's CAN_SendStatus/CAN_SendHeartbeat, these
// don't transmit anywhere themselves - they stash the response frame into
// a module-level buffer (bootloader_protocol.c) that bootloader_main.c's
// own SPI1 loop reads and sends back to the CM5 on the NEXT transaction.
// Only meaningful while processing a SPI_TARGET_SELF frame - see this
// file's own bootloader_main.c for the full request/response shape.
void CAN_SendStatus(uint8_t status);
void CAN_SendHeartbeat(uint8_t status, uint8_t progress_percent);

void HandleVersionQuery(void);
void HandleErrorCounterQuery(void);
void HandleAuthorizeDowngrade(uint8_t *data);
void HandleReadbackStart(void);

uint8_t ApplicationIsValid(void);
void JumpToApplication(void);

void HandleStartUpdate(uint8_t *data);
void HandleHmacChunk(uint8_t *data);
void HandleData(uint8_t *data, uint32_t dlc);
void HandleEndUpdate(uint8_t *data);

// Reads whatever CAN_SendStatus/CAN_SendHeartbeat/etc. most recently
// stashed (see this file's own header) into a caller-supplied SpiOtaFrame_t
// - bootloader_main.c calls this right before every SPI1 transaction to
// build the frame it sends back. Returns 1 if there was something new
// since the last read, 0 if nothing changed (caller keeps sending the
// previous frame - SPI is master-clocked, so *something* has to go out
// every transaction regardless).
uint8_t Protocol_TakePendingResponse(SpiOtaFrame_t *out);

// -----------------------------------------------------------------------
// Relay helpers - bootloader_main.c's own SPI1 loop calls exactly one of
// these per received frame whose target_tier isn't SPI_TARGET_SELF. Each
// blocks (with IWDG refresh) for a bounded time waiting for the relayed
// target to answer, then fills *out with whatever it got (or a synthesized
// STATUS_ERROR/timeout frame if nothing came back in time) - so the SPI1
// loop always has SOMETHING to send back next transaction either way.
// -----------------------------------------------------------------------
void Relay_ToStackA(uint8_t slot, const SpiOtaFrame_t *in, SpiOtaFrame_t *out);
void Relay_ToCM7(const SpiOtaFrame_t *in, SpiOtaFrame_t *out);

#endif // BOOTLOADER_PROTOCOL_H
