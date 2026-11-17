// =============================================================================
// HYDRA-UMC Kinematic Brain Bootloader (CM4) - shared types/defines/externs
// Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
// GPL-3.0-or-later - see repo root LICENSE
//
// This core is the CAN-OTA "gateway" - the only externally-reachable point
// of the whole 4-tier chain (docs/architecture.md section 1): it owns
// SPI1 (the physical link to the CM5, README.md section 10) AND FDCAN1
// ("STACK A", README.md section 6), so every update for every tier passes
// through here first. Three destinations, one SPI1 frame format:
//   - TARGET_SELF   (0): this core's own Bank 2 - handled directly,
//     same self-update state machine as every other bootloader in this
//     project (see bootloader_protocol.c).
//   - TARGET_CM7    (1): relayed over the CM7<->CM4 IPC mailbox
//     (../../Common/ipc_mailbox.h) - this core never touches CM7's own
//     Bank 1 flash directly, it just forwards frames and relays responses.
//   - TARGET_STACKA (2): relayed onto FDCAN1 at the target slot's own base
//     ID (docs/architecture.md section 4) - covers Robot Controller Boards
//     directly, and (via THEIR own RELAY_SEND/RELAY_RECV tunnel, section 5)
//     URTC Tool Heads and Advanced Expansion Boards too, with zero
//     additional logic here: this core doesn't need to know or care that a
//     RELAY_SEND frame ultimately reaches a 3rd or 4th tier - it just
//     forwards the same opaque OFS_RELAY_SEND/OFS_RELAY_RECV frame types
//     like any other STACK A traffic.
// =============================================================================
#ifndef BOOTLOADER_COMMON_H
#define BOOTLOADER_COMMON_H

#include "stm32h7xx_hal.h"

extern SPI_HandleTypeDef   hspi1;
extern FDCAN_HandleTypeDef hfdcan1;
extern IWDG_HandleTypeDef  hiwdg;

// -----------------------------------------------------------------------
// Flash layout - Bank 2, same 8-sectors-x-128KB shape and same "metadata
// gets its own sector" reasoning as CM7's own bootloader_common.h (see
// that file's own comment) - offset +0x100000 from Bank 1 throughout.
// -----------------------------------------------------------------------
// FLASH_SECTOR_SIZE (128 KB) is already defined by CMSIS itself
// (stm32h757xx.h) - not redefined here, same precedent as FLASH_PAGE_SIZE
// in the G474 bootloader's own bootloader_common.h.
#define BANK2_BASE           0x08100000UL
#define BOOT_SECTOR_ADDR     0x08100000UL // sector 0
#define METADATA_ADDR        0x08120000UL // sector 1, own dedicated sector
#define MAIN_APP_ADDR        0x08140000UL // sectors 2-4
#define BACKUP_APP_ADDR      0x081A0000UL // sectors 5-7
// HYDRA_META_AB (off as it ships) keeps the metadata in two sectors that take turns (meta_ab.h): sector 1 as always and a second one, METADATA_B_ADDR, which is
// the last sector of the application backup slot, so the longest application is then 256 KB.
#ifndef HYDRA_META_AB
#define HYDRA_META_AB 0
#endif
#if HYDRA_META_AB
#define METADATA_B_ADDR      0x081E0000UL // sector 7, the second metadata sector
#define APP_MAX_SIZE         (256UL * 1024UL) // 2 sectors
#else
#define APP_MAX_SIZE         (384UL * 1024UL)
#endif
#define METADATA_MAGIC_VALID 0x55415034UL // "4PAU" - CM4, distinct from CM7's "CPAU" and G474's "UPAU"
#define FLASH_PAGE_SIZE       0x800UL // 2048 bytes - logical OTA transfer-chunk size, not hardware erase granularity (see ../../CM7/boot/bootloader_flash.c's own header, identical reasoning here)

#define THIS_HARDWARE_ID       0x48374334UL // "H7C4" - STM32H745ZIT6 CM4 core, HYDRA-UMC Kinematic Brain revision 1
// FIRMWARE_VERSION_* (this core's application) and BOOTLOADER_VERSION_*
// (this bootloader) are BOTH incremental - PATCH bumps by 1 on every real
// build of that component, via bump_version.py, called automatically by
// tools/build_firmware.py before this header is compiled in. Odometer carry
// rule (PATCH past 9 -> MINOR+1, PATCH resets to 0) - full reasoning in
// src/mcu_stm32g474/boot/bootloader_common.h's own header comment, same
// rule applies here verbatim.
#define FIRMWARE_VERSION_MAJOR 0
#define FIRMWARE_VERSION_MINOR 1
#define FIRMWARE_VERSION_PATCH 7

#define BOOTLOADER_VERSION_MAJOR 0
#define BOOTLOADER_VERSION_MINOR 1
#define BOOTLOADER_VERSION_PATCH 8

// Image authentication: the bootloader holds no secret and no key of its own. An update is installed only when its Ed25519 signature verifies under a
// PUBLIC key of the trust table (bootloader_trust.h); with an empty table nothing installs. What the metadata keeps to notice a damaged flash is a plain
// SHA-256 of the installed application.

// STACK A slot addressing (docs/architecture.md section 4) - this core
// (not a Robot Controller Board itself) is the one that OWNS this
// addressing scheme, computing target slot base IDs to relay to rather
// than reading its own BOARD_ID the way src/mcu_stm32g474/boot/
// bootloader_common.h's own copy of these constants does (that board reads
// a local DIP switch; this core has none to read - the target slot comes
// from the SPI1 frame's own target_slot field instead, see
// bootloader_protocol.c's own Relay_ToStackA()).
#define CAN_ID_STACKA_BASE 0x600UL
#define STACKA_SLOT_WINDOW 0x40UL

// Same offset table as every other bootloader in this project (docs/
// architecture.md section 4) - this core uses it 3 ways at once: as SPI1
// frame_type bytes (talking to the CM5), as FDCAN1 ID offsets (talking to
// STACK A), and as IpcFrame_t frame_type bytes (talking to CM7) - the
// whole point of keeping ONE numeric offset space is that this gateway
// core can translate between all three without a lookup table.
#include "boot_protocol_ids.h"  // OFS_*, STATUS_*, VERIFY_FAIL_REASON_*, META_STATE_*
#define OFS_RELAY_SEND                                      0x12 // opaque to this core - just forwarded, see this file's own header
#define OFS_RELAY_RECV                                        0x13 // opaque to this core - just forwarded
// One emitter per identifier (tools/can_ids.py): the controller board answers on its own identifiers, not on the one the request came in on.
#define OFS_AXIS_STEP_TELEMETRY                               0x11 // sent BY the controller board's application (axis step telemetry and, with the switch on, the health frame)
#define OFS_AXIS_STATUS_RESPONSE                              0x16 // sent BY the controller board's application
#define OFS_POLL_STATUS                                       0x17 // SPI frame type from the CM5, DLC=1: the frame type to wait for; nothing is transmitted on CAN
#define OFS_RELAY_RECV_RESPONSE                               0x18 // sent BY the controller board's application

typedef struct {
    uint32_t magic;
    uint32_t state;
    uint32_t hardware_id;
    uint32_t version_major;
    uint32_t version_minor;
    uint32_t size;
    uint32_t crc32;
    uint8_t  sha256[32];   // SHA-256 of the installed application (no key: it detects a damaged flash; the signature was checked at the installation)
    // appended at the end - see mcu_stm32g474/boot/
    // bootloader_common.h's own field-level comment for why this is
    // always a real, explicit 0 on a fresh install.
    uint32_t boot_attempts;
    // The patch of the installed version, appended after boot_attempts so the older fields keep their offsets. A page written before
    // this field existed holds erased flash here (0xFFFFFFFF): METADATA_PATCH() reads that as 0.
    uint32_t version_patch;
    // The floor of the signed rollback counter: an image whose counter is below it is refused. Appended last so the older fields keep their offsets.
    uint32_t counter;
} FirmwareMetadata_t;
#define METADATA_PATCH(meta) (((meta).version_patch <= 0xFFFFu) ? (uint16_t)(meta).version_patch : (uint16_t)0u)

// The metadata is an append-only log of records (generation, state, CRC-32), not one record that is erased and written again: the valid record with the
// highest generation is the state, a record cut by a power loss fails its CRC and is ignored, so the previous state stays in force.
typedef struct {
    uint32_t generation;
    FirmwareMetadata_t meta;
    uint32_t reserved[4];  // pads the record to three flash words of 32 bytes
    uint32_t crc;          // CRC-32 (zlib) of everything before it
} MetaRecord_t;
#define META_RECORD_SIZE   96u
#define META_RECORD_SLOTS  (0x20000u / META_RECORD_SIZE)   // records in the dedicated 128 KB sector

void CAN_SendHeartbeat(uint8_t status, uint8_t progress_percent);

// -----------------------------------------------------------------------
// SPI1 frame format (README.md section 10's own 128-byte frame size,
// reused here rather than defining a different one for bootloader mode -
// same DMA/buffer sizing works for both) - see this file's own header for
// the 3-way routing this enables. Fixed layout, rest of the 128 bytes
// reserved/padded 0xFF for future growth (matches every other frame format
// in this project leaving explicit reserved space rather than packing
// tight).
// -----------------------------------------------------------------------
#define SPI_FRAME_SIZE       128
#define SPI_TARGET_SELF      0
#define SPI_TARGET_CM7       1
#define SPI_TARGET_STACKA    2

typedef struct {
    uint8_t target_tier; // SPI_TARGET_*
    uint8_t target_slot; // 0-7, meaningful only when target_tier == SPI_TARGET_STACKA
    uint8_t frame_type;  // OFS_*
    uint8_t dlc;          // 0-8
    uint8_t payload[8];
    uint8_t _reserved[SPI_FRAME_SIZE - 12];
} SpiOtaFrame_t; // 128 bytes exactly

#endif // BOOTLOADER_COMMON_H
