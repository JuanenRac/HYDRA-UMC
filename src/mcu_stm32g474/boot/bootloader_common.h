// =============================================================================
// HYDRA-UMC Robot Controller Board Bootloader - shared types/defines/externs
// Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
// GPL-3.0 - see repo root LICENSE
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
// (stm32g4xx_hal_flash.h) - not redefined here, same precedent URTC's own
// bootloader_common.h documents for why (matches STM32G4 Cat.2/3 page size
// in single-bank/DBANK=0 mode).

// -----------------------------------------------------------------------
// Firmware identity - rejects an image built for different hardware or
// declared incompatible, before ever trusting its CRC/HMAC.
// -----------------------------------------------------------------------
#define THIS_HARDWARE_ID       0x47343031UL // "G401" - STM32G474RET6, HYDRA-UMC Robot Controller Board revision 1

// FIRMWARE_VERSION_* describes the APPLICATION image (firmware/
// mcu_stm32g474/STM32G474RE_main.c), not this bootloader itself.
// Ecosystem-wide versioning policy for THIS repo (unlike sibling repo
// URTC, where only bootloaders are incremental): ALL 6 HYDRA-UMC
// components, including this one, are incremental - PATCH bumps by
// exactly 1 on every real build that produces a new binary for this
// application, via bump_version.py, called automatically as the first
// step of "6. Robot Controller Board application" in build_firmware.sh/
// .bat, BEFORE this header is compiled in. "Odometer" carry rule: PATCH
// past 9 resets to 0 and MINOR increments (e.g. 0.1.9 -> 0.2.0, never
// 0.1.10) - same rule BOOTLOADER_VERSION_* below already used. Also
// embedded in the output filename (HYDRA_RCB_APP_v0.0.0.bin).
#define FIRMWARE_VERSION_MAJOR 0
#define FIRMWARE_VERSION_MINOR 0
#define FIRMWARE_VERSION_PATCH 8

// BOOTLOADER_VERSION_* describes THIS bootloader binary itself. Versioning
// convention (matches URTC's own shape, but NOT its manual-bump timing -
// see bump_version.py's own header comment): PATCH increments on every
// real build of this partitioned bootloader source (0-9), then MINOR
// increments and PATCH resets to 0. Also embedded in the output filename
// by build_firmware.sh (HYDRA_RCB_BOOTLOADER_v0.0.0.bin).
#define BOOTLOADER_VERSION_MAJOR 0
#define BOOTLOADER_VERSION_MINOR 0
#define BOOTLOADER_VERSION_PATCH 8

// -----------------------------------------------------------------------
// HMAC-SHA256 signing key - PLACEHOLDER, same caveat as URTC's own key
// (bootloader_crypto.c): change before relying on this for anything real,
// and keep whatever build/signing tool prepares HYDRA-UMC update images in
// sync. Deliberately a DIFFERENT key than URTC's own (see bootloader_crypto.c)
// - these are two separate trust domains, a URTC-signed image must never
// verify against a HYDRA-UMC board and vice versa.
// -----------------------------------------------------------------------
extern const uint8_t HMAC_KEY[32];

// -----------------------------------------------------------------------
// Slot addressing (docs/architecture.md section 4) - this board's own
// FDCAN1 slot base ID, computed once at boot from the BOARD_ID[2:0] LOCAL
// DIP-switch pins (docs/PINOUT_STM32G474_ROBOT_CONTROLLER.TXT section 1c) -
// manually set by whoever installs this board, NOT derived from the STACK
// A connector or physical stack position (docs/PINOUT_STACKA_CONNECTOR.TXT
// section 1 has the reasoning: every board is the same interchangeable
// PCB, so a connector-position-derived scheme needs either per-position
// board variants or a fragile power-up ripple sequence - a local DIP
// switch avoids both). Same binary runs on every board regardless of which
// address its own switch is set to.
// -----------------------------------------------------------------------
#define CAN_ID_STACKA_BASE 0x600UL
#define STACKA_SLOT_WINDOW 0x20UL

// Reads BOARD_ID[2:0] (PC0=LSB, PC1, PC2=MSB) and returns this board's own
// 0x600+N*0x20 base - called once by bootloader_main.c at startup, result
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
#define OFS_ENTER_BOOTLOADER  0x00 // sent to the RUNNING APPLICATION - handled in app firmware, not here
#define OFS_START_UPDATE      0x01 // DLC=8: big-endian total size (4) + big-endian HardwareID (4)
#define OFS_DATA               0x02 // up to 8 bytes of firmware data, sequential - goes to BACKUP slot only
#define OFS_PAGE_ACK           0x03 // sent BY this bootloader: DLC=4, big-endian page index
#define OFS_END_UPDATE         0x04 // DLC=8: big-endian CRC32 (4) + version major/minor (2+2)
#define OFS_STATUS              0x05 // sent BY this bootloader: DLC=1 (or 2 with a verify-fail reason)
#define OFS_HEARTBEAT           0x06 // sent BY this bootloader every ~1s: DLC=2, status + progress%
#define OFS_HMAC_CHUNK           0x07 // DLC=8 x4, right after START_UPDATE, before the first DATA frame
#define OFS_QUERY_VERSION        0x08 // sent to either app or bootloader, whichever is running
#define OFS_VERSION_RESPONSE      0x09 // DLC=8: byte0(0=app,1=boot)+HardwareID(4)+verMajor(2)+verMinor(1)
#define OFS_BOOTLOADER_VERSION     0x0A // sent ONLY by the bootloader, alongside +0x09
#define OFS_QUERY_ERROR_COUNTERS   0x0B // sent to either app or bootloader
#define OFS_ERROR_COUNTERS_RESPONSE 0x0C // DLC=2: TEC + REC, read from the FDCAN peripheral's own PSR register
#define OFS_AUTHORIZE_DOWNGRADE     0x0D // DLC=4, magic 0xD0,0x9E,0x12,0xAD - one-shot anti-rollback bypass
#define OFS_BACKUP_READ_REQUEST      0x0E // any DLC starts a main-slot readback
#define OFS_BACKUP_READ_RESPONSE      0x0F // first reply DLC=4 (total size), then DLC=8 raw bytes
#define OFS_AXIS_STATUS                0x10 // application-only, not this bootloader
#define OFS_AXIS_STEP_TELEMETRY         0x11 // application-only, not this bootloader
#define OFS_RELAY_SEND                   0x12 // application-only (Tier 2/3 tunnel), not this bootloader
#define OFS_RELAY_RECV                    0x13 // application-only (Tier 2/3 tunnel), not this bootloader
// Found missing during implementation (not in the original docs/
// architecture.md section 4 table - added there alongside this file): the
// readback direction needs its OWN page-ack offset, distinct from +0x03's
// write-direction PAGE_ACK and from +0x0F's own data-response frames -
// URTC's own protocol has exactly this same 3-way split (0x7F3 write-ack /
// 0x7FE read-data / 0x7FF read-ack, CANBUS.TXT), which the original table
// here collapsed into 2 offsets by mistake. Master->bootloader, DLC=4,
// big-endian page index - same convention as +0x03, opposite direction.
#define OFS_BACKUP_READ_PAGE_ACK          0x14

// Status/verify-fail codes - identical values to URTC's own, so a shared
// host-side decoder (HYDRA-UMC-STUDIO's canOta.ts) can use one status enum
// across every tier instead of a per-tier mapping.
#define STATUS_LISTENING     0x01
#define STATUS_ERASING       0x02
#define STATUS_RECEIVING     0x03
#define STATUS_VERIFYING     0x06
#define STATUS_COPYING       0x07
#define STATUS_VERIFY_OK     0x04
#define STATUS_VERIFY_FAIL   0x05
#define VERIFY_FAIL_REASON_INCOMPLETE  0x01
#define VERIFY_FAIL_REASON_CRC32       0x02
#define VERIFY_FAIL_REASON_HMAC        0x03
#define VERIFY_FAIL_REASON_HARDWARE_ID 0x04
#define VERIFY_FAIL_REASON_ROLLBACK    0x05
#define STATUS_ERROR          0xFF

// -----------------------------------------------------------------------
// Firmware metadata (single 2K page, one struct) - byte-identical layout to
// URTC's own, deliberately, so tooling that already parses one can parse
// the other with zero changes.
// -----------------------------------------------------------------------
#define META_STATE_APP_VALID     1
#define META_STATE_COPY_PENDING  2

typedef struct {
    uint32_t magic;
    uint32_t state;
    uint32_t hardware_id;
    uint32_t version_major;
    uint32_t version_minor;
    uint32_t size;
    uint32_t crc32;
    uint8_t  hmac[32];
} FirmwareMetadata_t;

void CAN_SendHeartbeat(uint8_t status, uint8_t progress_percent);

#endif // BOOTLOADER_COMMON_H
