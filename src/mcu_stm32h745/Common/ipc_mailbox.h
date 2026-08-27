// =============================================================================
// HYDRA-UMC Kinematic Brain - CM7<->CM4 shared-memory IPC mailbox
// Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
// GPL-3.0 - see repo root LICENSE
//
// Both cores' bootloaders (and, later, applications) include this same
// header - it describes ONE piece of shared state living in SRAM4 (D3
// domain, 64 KB @ 0x38000000), the only RAM region both cores can see:
// CM7 uses D1-domain AXI SRAM for its own .data/.bss, CM4 uses D2-domain
// SRAM1-3 for its own - neither can see the other's private RAM at all.
//
// USE (CAN-OTA, bootloader context): the CM4 bootloader owns the physical
// SPI1 link to the CM5 (docs/architecture.md section 3) - it's the only
// core that can be flashed directly from outside the chip. Flashing CM7
// itself has to be relayed one hop further, over this mailbox: CM4 writes
// an IpcCommandFrame_t here (the same frame-type-byte/DLC/payload shape
// used on SPI1 and FDCAN1 - see boot/bootloader_common.h's own OFS_*), then
// releases HSEM channel IPC_HSEM_CMD_ID; CM7's bootloader (polling that
// semaphore in its own main loop, same "no interrupts, nothing else
// competing for CPU time" pattern URTC's own bootloader uses for its CAN
// peripheral) picks it up, runs it through the SAME protocol handler logic
// as every other tier, writes an IpcResponseFrame_t back, and releases
// IPC_HSEM_RESP_ID for CM4 to relay back over SPI1 to the CM5.
//
// Ownership is asymmetric: only CM4 ever writes .cmd / reads .resp; only
// CM7 ever reads .cmd / writes .resp - no locking beyond the two HSEM
// channels is needed because each field only ever has ONE writer.
//
// CACHE COHERENCY: NOT a race as currently built - CM7's
// bootloader never calls SCB_EnableDCache() (grepped, none exist anywhere
// under CM7/boot/ - SystemClock_Config() there is still an explicit TODO,
// see that file's own header), and a Cortex-M7 resets with its D-Cache
// OFF by default, so there is currently nothing caching a stale read of
// this SRAM4 region at all. This WOULD become a real bug the instant
// someone enables CM7's D-Cache for performance later without also
// either (a) marking this 0x38000000 region non-cacheable via the MPU,
// or (b) adding an explicit SCB_InvalidateDCache_by_Addr() on IPC_MAILBOX
// before every .cmd read (and a clean+invalidate before every .resp
// write) - CM4 has no cache at all, so only the CM7 side would ever need
// this. Left as a comment, not a pre-emptive code change, because adding
// cache-maintenance calls against a cache that isn't enabled yet is dead
// code that could mislead a future reader into thinking this is already
// handled - whoever enables CM7's D-Cache should read this comment and
// add the real fix in that same change, not before.
// =============================================================================
#ifndef IPC_MAILBOX_H
#define IPC_MAILBOX_H

#include <stdint.h>

#define IPC_MAILBOX_BASE 0x38000000UL

// HSEM channel indices (of this chip's 32 available) - HSEM0 signals a
// fresh command is waiting in .cmd (CM4 -> CM7), HSEM1 signals a fresh
// response is waiting in .resp (CM7 -> CM4). Taking a channel (HAL_HSEM_
// FastTake) is how the WRITER claims exclusive access to post a new frame;
// the READER never takes it, only checks HAL_HSEM_IsSemTaken() and, once
// it's done consuming the frame, calls HAL_HSEM_Release() to both free the
// channel for the next post AND signal "consumed" back implicitly (the
// writer's own next FastTake attempt succeeding IS the consumed signal -
// no separate ack field needed for a single-producer/single-consumer pair).
#define IPC_HSEM_CMD_ID  0
#define IPC_HSEM_RESP_ID 1

// 16 bytes each - deliberately the same frame_type/dlc/payload[8] shape as
// every other tier's protocol frame (SPI1 to the CM5, FDCAN1 to STACK A),
// so HandleData/HandleStartUpdate/etc. in each core's own bootloader_
// protocol.c can be called with this frame's fields directly, no reshaping.
typedef struct {
    volatile uint8_t frame_type; // OFS_* from bootloader_common.h
    volatile uint8_t dlc;        // 0-8, meaningful bytes in payload[]
    volatile uint8_t payload[8];
    volatile uint8_t _reserved[6];
} IpcFrame_t; // 16 bytes

typedef struct {
    IpcFrame_t cmd;   // CM4 writes, CM7 reads - guarded by IPC_HSEM_CMD_ID
    IpcFrame_t resp;  // CM7 writes, CM4 reads - guarded by IPC_HSEM_RESP_ID
} IpcMailbox_t; // 32 bytes total - a tiny sliver of SRAM4's own 64 KB

#define IPC_MAILBOX ((IpcMailbox_t*)IPC_MAILBOX_BASE)

#endif // IPC_MAILBOX_H
