// =============================================================================
// HYDRA-UMC Robot Controller Board Bootloader - shared types/defines/externs
// Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
// GPL-3.0-or-later - see repo root LICENSE
//
// Ported from URTC's own proven bootloader (sibling repo, src/F303-master/
// boot/bootloader_common.h) - same struct layout, same CAN_ID_* command
// *offsets*, same status/verify-fail codes, same metadata scheme. What's
// different from URTC: this board sits on a shared, slot-addressed bus
// (FDCAN1 "STACK A", up to 8 boards) instead of URTC's one-board-per-segment
// bxCAN, so every ID below is a SLOT-RELATIVE OFFSET added to this board's
// own SlotBaseId() (section "Slot addressing" below), not a fixed absolute
// ID - see docs/architecture.md section 4 for the full addressing design
// this file implements.
// =============================================================================
#ifndef BOOTLOADER_COMMON_H
#define BOOTLOADER_COMMON_H

#include "stm32g4xx_hal.h"

// -----------------------------------------------------------------------
// Shared peripheral handles - defined once in bootloader_main.c, declared
// extern here so every module that needs one can see it without each
// module guessing at ownership.
// -----------------------------------------------------------------------
extern FDCAN_HandleTypeDef hfdcan1;
extern IWDG_HandleTypeDef  hiwdg;

// -----------------------------------------------------------------------
// Flash layout - matches ../STM32G474RETx_APP.ld / ../STM32G474RETx_BOOTLOADER.ld
// and the table in ../README.md ("Flash layout" section).
// -----------------------------------------------------------------------
#define MAIN_APP_ADDR        0x08009000UL
#define BACKUP_APP_ADDR      0x08044000UL
#define APP_MAX_SIZE         (236UL * 1024UL)
#define METADATA_ADDR        0x08008000UL
#define METADATA_MAGIC_VALID 0x55415055UL // "UPAU" - distinct from URTC's own "APAU" so a metadata page from one project is never mistaken for the other's
// FLASH_PAGE_SIZE (2048 bytes) is already defined by the HAL itself
// (stm32g4xx_hal_flash.h) - not redefined here. It is the page size of the
// dual-bank layout (option bit DBANK = 1, the factory default): two banks of
// 128 pages, numbered per bank - see flash_geometry.h.

// -----------------------------------------------------------------------
// Firmware identity - rejects an image built for different hardware or
// declared incompatible, before ever trusting its CRC/signature.
// -----------------------------------------------------------------------
#define THIS_HARDWARE_ID       0x47343031UL // "G401" - STM32G474RET6, HYDRA-UMC Robot Controller Board revision 1

// FIRMWARE_VERSION_* describes the APPLICATION image (firmware/
// mcu_stm32g474/STM32G474RE_main.c), not this bootloader itself.
// Ecosystem-wide versioning policy for THIS repo (unlike sibling repo
// URTC, where only bootloaders are incremental): ALL 6 HYDRA-UMC
// components, including this one, are incremental - PATCH bumps by
// exactly 1 on every real build that produces a new binary for this
// application, via bump_version.py, called automatically as the first
// step of "6. Robot Controller Board application" in tools/build_firmware.py/
// .bat, BEFORE this header is compiled in. "Odometer" carry rule: PATCH
// past 9 resets to 0 and MINOR increments (e.g. 0.1.9 -> 0.2.0, never
// 0.1.10) - same rule BOOTLOADER_VERSION_* below already used. Also
// embedded in the output filename (HYDRA_RCB_APP_v0.0.0.bin).
#define FIRMWARE_VERSION_MAJOR 0
#define FIRMWARE_VERSION_MINOR 3
#define FIRMWARE_VERSION_PATCH 0
#define FIRMWARE_VERSION_BUILD 0

// BOOTLOADER_VERSION_* describes THIS bootloader binary itself. Versioning
// convention (matches URTC's own shape, but NOT its manual-bump timing -
// see bump_version.py's own header comment): PATCH increments on every
// real build of this partitioned bootloader source (0-9), then MINOR
// increments and PATCH resets to 0. Also embedded in the output filename
// by tools/build_firmware.py (HYDRA_RCB_BOOTLOADER_v0.0.0.bin).
#define BOOTLOADER_VERSION_MAJOR 0
#define BOOTLOADER_VERSION_MINOR 1
#define BOOTLOADER_VERSION_PATCH 9

// Image authentication: the bootloader holds no secret and no key of its own. An update is installed only when its Ed25519 signature verifies under a
// PUBLIC key of the trust table (bootloader_trust.h); with an empty table nothing installs. What the metadata keeps to notice a damaged flash is a plain
// SHA-256 of the installed application.

// -----------------------------------------------------------------------
// Slot addressing (docs/architecture.md section 4) - this board's own
// FDCAN1 slot base ID, computed once at boot from the BOARD_ID[2:0] LOCAL
// DIP-switch pins (docs/PINOUT_STM32G474_ROBOT_CONTROLLER.md section 1c) -
// manually set by whoever installs this board, NOT derived from the STACK
// A connector or physical stack position (docs/PINOUT_STACKA_CONNECTOR.md
// section 1 has the reasoning: every board is the same interchangeable
// PCB, so a connector-position-derived scheme needs either per-position
// board variants or a fragile power-up ripple sequence - a local DIP
// switch avoids both). Same binary runs on every board regardless of which
// address its own switch is set to.
// -----------------------------------------------------------------------
#define CAN_ID_STACKA_BASE 0x600UL
#define STACKA_SLOT_WINDOW 0x40UL

// Reads BOARD_ID[2:0] (PC0=LSB, PC1, PC2=MSB) and returns this board's own
// 0x600+N*0x40 base - called once by bootloader_main.c at startup, result
// cached and passed to every protocol function that needs it (not re-read
// per frame - a DIP switch can't change while the board is powered).
uint32_t ReadSlotBaseId(void);

// -----------------------------------------------------------------------
// Bootloader command offsets (docs/architecture.md section 4 table) - added
// to a slot base ID to form the actual FDCAN1 standard ID for every frame
// this bootloader sends or listens for. Offsets +0x00..+0x0F are a direct,
// verbatim re-based copy of URTC's own fixed 0x7F0-0x7FF (see that repo's
// own docs/CANBUS.TXT) - same command semantics, same DLC/payload layout,
// only the base address differs. +0x10/+0x11 (axis telemetry) and +0x12/
// +0x13 (Tier 2/3 relay tunnel) are new, not implemented by this bootloader
// itself (the relay tunnel is a RUNTIME-application concern, handled after
// this bootloader hands off to the app - see docs/architecture.md section
// 5) - listed here only so the full 0x00-0x1F window is documented in one
// place, matching the offset table's own source of truth.
// -----------------------------------------------------------------------
// The offsets, status codes, verify-fail reasons and metadata states shared by the three bootloaders (with the full
// comments of each) are in boot_protocol_ids.h; below, only the ones this chip alone carries.
#include "boot_protocol_ids.h"  // OFS_*, STATUS_*, VERIFY_FAIL_REASON_*, META_STATE_*
#define OFS_AXIS_STATUS                0x10 // application-only, not this bootloader
#define OFS_AXIS_STEP_TELEMETRY         0x11 // application-only, not this bootloader
#define OFS_RELAY_SEND                   0x12 // application-only (Tier 2/3 tunnel), not this bootloader
#define OFS_RELAY_RECV                    0x13 // application-only (Tier 2/3 tunnel), not this bootloader
// One emitter per identifier (tools/can_ids.py): the controller board answers on its own identifiers, not on the one the request came in on.
#define OFS_AXIS_STATUS_RESPONSE          0x16 // sent BY the application (controller board), DLC=2: answer to OFS_AXIS_STATUS
#define OFS_POLL_STATUS                   0x17 // host <-> CM4 gateway only (SPI frame type): never put on the CAN bus; DLC=1, the frame type to wait for
#define OFS_RELAY_RECV_RESPONSE           0x18 // sent BY the application (controller board): fragments answering OFS_RELAY_RECV

// -----------------------------------------------------------------------
// Firmware metadata (single 2K page, one struct). Layout WAS byte-identical
// to URTC's own (deliberately, so tooling that already parses one can parse
// the other with zero changes) - 's new `boot_attempts` field
// below is appended at the END, so URTC's own metadata (which does not have
// it yet) still parses the fields it knows about correctly; only this
// project's own tooling reading the new field needs updating. URTC gaining
// the SAME field later (real, separate future work - not done in this
// change) would restore full byte-identical parity.
// -----------------------------------------------------------------------

typedef struct {
    uint32_t magic;
    uint32_t state;
    uint32_t hardware_id;
    uint32_t version_major;
    uint32_t version_minor;
    uint32_t size;
    uint32_t crc32;
    uint8_t  sha256[32];   // SHA-256 of the installed application (no key: it detects a damaged flash; the signature was checked at the installation)
    // appended at the end. Every real writer already
    // zero-initializes the whole struct first (`FirmwareMetadata_t pending
    // = {0};` in HandleEndUpdate) before setting the fields it cares about,
    // so this is always a real, explicit 0 on a fresh install - never an
    // uninitialized/garbage read. A metadata page with no valid magic at
    // all (a genuinely blank chip) never reaches this field either:
    // ApplicationIsValid()'s own magic check already fails first.
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
    uint32_t crc;         // CRC-32 (zlib) of the generation and the metadata
} MetaRecord_t;
#define META_RECORD_SIZE   80u                      // a multiple of 8: the programming unit of this flash is the double-word
#define META_RECORD_SLOTS  (2048u / META_RECORD_SIZE)   // records in one page; the log alternates between two pages (METADATA_ADDR and the next)

void CAN_SendHeartbeat(uint8_t status, uint8_t progress_percent);

#endif // BOOTLOADER_COMMON_H
