// =============================================================================
// HYDRA-UMC Kinematic Brain Bootloader (CM7) - update state machine and
// application-jump declarations
// Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
// GPL-3.0 - see repo root LICENSE
//
// Ported from URTC's own proven bootloader_protocol.h via HYDRA-UMC's own
// G474 port - same function set, same command semantics. No Protocol_Init()
// here (unlike G474's slot-addressed version): this core is reached only
// through the CM7<->CM4 mailbox (../../Common/ipc_mailbox.h), a fixed
// point-to-point channel with nothing to address.
// =============================================================================
#ifndef BOOTLOADER_PROTOCOL_H
#define BOOTLOADER_PROTOCOL_H

#include <stdint.h>

extern uint32_t update_total_size;
extern uint32_t update_bytes_received;
extern uint8_t  update_in_progress;
extern uint32_t update_last_activity_tick;
extern uint8_t  update_failed;

// Unlike every other tier, these two don't transmit on a bus themselves -
// they write an IpcFrame_t into the mailbox's own .resp slot and release
// IPC_HSEM_RESP_ID, for CM4's own gateway loop to relay onward. Named the
// same as every other bootloader's functions anyway (not IPC_SendStatus/
// IPC_SendHeartbeat) so bootloader_protocol.c's own handler bodies, ported
// from the same URTC-derived source, don't need renaming throughout.
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
