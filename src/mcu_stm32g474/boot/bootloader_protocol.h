// =============================================================================
// HYDRA-UMC Robot Controller Board Bootloader - CAN-OTA state machine and
// application-jump declarations
// Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
// GPL-3.0 - see repo root LICENSE
//
// Ported from URTC's own proven bootloader_protocol.h - same function set,
// same command semantics (docs/architecture.md section 4's own offset
// table is a verbatim re-base of URTC's own 0x7F0-0x7FF). What's added:
// Protocol_Init() below, since every ID this file sends/listens for is
// slot-relative (base + offset) rather than a fixed absolute ID - the base
// has to come from somewhere before any of the rest of this can run.
// =============================================================================
#ifndef BOOTLOADER_PROTOCOL_H
#define BOOTLOADER_PROTOCOL_H

#include <stdint.h>

// Called once by bootloader_main.c at startup, before anything else in
// this file - stores this board's own FDCAN1 slot base ID (from
// ReadSlotBaseId(), bootloader_common.h) for every function below to use.
void Protocol_Init(uint32_t slot_base_id);

// These 5 are read (and, for the inactivity-timeout abort, written)
// directly by main()'s own heartbeat-progress and timeout logic - genuinely
// shared state, not just accessed through the functions below.
extern uint32_t update_total_size;
extern uint32_t update_bytes_received;
extern uint8_t  update_in_progress;
extern uint32_t update_last_activity_tick;
extern uint8_t  update_failed;

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

#endif // BOOTLOADER_PROTOCOL_H
