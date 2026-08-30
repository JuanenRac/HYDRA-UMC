// =============================================================================
// HYDRA-UMC Kinematic Brain Bootloader (CM7) - shared types/defines/externs
// Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
// GPL-3.0 - see repo root LICENSE
//
// Ported from URTC's own proven bootloader (sibling repo) via HYDRA-UMC's
// own G474 port (../../../mcu_stm32g474/boot/bootloader_common.h) - same
// struct layout, same command *offsets*, same status/verify-fail codes,
// same metadata scheme. Two real differences from the G474 version:
//   1. This core has NO external bus of its own at all (no SPI1 - that's
//      CM4's peripheral; no FDCAN - also CM4's). Every command this
//      bootloader ever sees arrives relayed through the CM7<->CM4 IPC
//      mailbox (../../Common/ipc_mailbox.h), not a physical bus this file
//      configures - there is no MX_xxx_Init() in this bootloader's own
//      main.c the way there is in G474's or CM4's.
//   2. Flash geometry: STM32H7 erases in 128 KB SECTORS
//      (FLASH_TYPEERASE_SECTORS), not 2 KB pages - see bootloader_flash.c's
//      own header for the full consequence of that.
// =============================================================================
#ifndef BOOTLOADER_COMMON_H
#define BOOTLOADER_COMMON_H

#include "stm32h7xx_hal.h"

extern IWDG_HandleTypeDef hiwdg;

// -----------------------------------------------------------------------
// Flash layout - Bank 1, 8 sectors x 128 KB (see ../STM32H745ZITx_CM7_
// BOOTLOADER.ld and ../../STM32H745ZITx_CM7_APP.ld for the full picture).
// Metadata gets its OWN dedicated sector (sector 1) rather than sharing
// sector 0 with the bootloader's own code, despite only needing ~40 bytes
// of it - found during implementation that erasing metadata inside the
// SAME sector the bootloader is actively executing from is a real hazard
// on this chip (CPU reading from a sector mid-erase can stall/fault per
// the datasheet), not just a style preference. F3/G4's much smaller 2KB/
// 4KB page granularity let metadata share a page without this problem;
// this chip's 128KB minimum erase unit doesn't allow the same trick
// safely. Costs one whole spare sector (128KB) that would otherwise have
// been "reserved" anyway - not a meaningful loss on a 2MB-flash part.
// -----------------------------------------------------------------------
// FLASH_SECTOR_SIZE (128 KB) is already defined by CMSIS itself
// (stm32h745xx.h) - not redefined here, same precedent as FLASH_PAGE_SIZE
// in the G474 bootloader's own bootloader_common.h.
#define BANK1_BASE           0x08000000UL
#define BOOT_SECTOR_ADDR     0x08000000UL // sector 0
#define METADATA_ADDR        0x08020000UL // sector 1, own dedicated sector
#define MAIN_APP_ADDR        0x08040000UL // sectors 2-4
#define BACKUP_APP_ADDR      0x080A0000UL // sectors 5-7
#define APP_MAX_SIZE         (384UL * 1024UL) // 3 sectors
#define METADATA_MAGIC_VALID 0x55415043UL // "CPAU" - CM7, distinct from G474's "UPAU" and CM4's "CPA4" so a metadata page from the wrong core/chip is never mistaken for this one's
#define FLASH_PAGE_SIZE       0x800UL // 2048 bytes - LOGICAL transfer-chunk size for the OTA protocol/page_buffer (matches every other tier for page-count/PAGE_ACK math), NOT this chip's hardware erase granularity - see bootloader_flash.c

#define THIS_HARDWARE_ID       0x48374337UL // "H7C7" - STM32H745ZIT6 CM7 core, HYDRA-UMC Kinematic Brain revision 1
// FIRMWARE_VERSION_* (this core's application) and BOOTLOADER_VERSION_*
// (this bootloader) are BOTH incremental - PATCH bumps by 1 on every real
// build of that component, via bump_version.py, called automatically by
// build_firmware.sh/.bat before this header is compiled in. Odometer carry
// rule (PATCH past 9 -> MINOR+1, PATCH resets to 0) - full reasoning in
// src/mcu_stm32g474/boot/bootloader_common.h's own header comment, same
// rule applies here verbatim.
#define FIRMWARE_VERSION_MAJOR 0
#define FIRMWARE_VERSION_MINOR 1
#define FIRMWARE_VERSION_PATCH 1

#define BOOTLOADER_VERSION_MAJOR 0
#define BOOTLOADER_VERSION_MINOR 1
#define BOOTLOADER_VERSION_PATCH 1

extern const uint8_t HMAC_KEY[32];

// Same offset table as G474's own bootloader_common.h (docs/architecture.md
// section 4) - reused here even though this core is never reached over
// FDCAN directly, because the IPC mailbox frame (../../Common/
// ipc_mailbox.h) reuses frame_type as this exact OFS_* byte, so the SAME
// protocol handler function signatures work unmodified regardless of
// whether the frame arrived over FDCAN1, SPI1, or the mailbox.
#define OFS_ENTER_BOOTLOADER  0x00
#define OFS_START_UPDATE      0x01
#define OFS_DATA               0x02
#define OFS_PAGE_ACK            0x03
#define OFS_END_UPDATE           0x04
#define OFS_STATUS                 0x05
#define OFS_HEARTBEAT                0x06
#define OFS_HMAC_CHUNK                 0x07
#define OFS_QUERY_VERSION                0x08
#define OFS_VERSION_RESPONSE               0x09
#define OFS_BOOTLOADER_VERSION               0x0A
#define OFS_QUERY_ERROR_COUNTERS               0x0B
#define OFS_ERROR_COUNTERS_RESPONSE               0x0C
#define OFS_AUTHORIZE_DOWNGRADE                     0x0D
#define OFS_BACKUP_READ_REQUEST                       0x0E
#define OFS_BACKUP_READ_RESPONSE                        0x0F
#define OFS_BACKUP_READ_PAGE_ACK                          0x14

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

// Sent by this bootloader as a mailbox response (not a CAN/SPI frame
// directly - CM4's own gateway logic relays it onward over whichever bus
// the original request came from).
void CAN_SendHeartbeat(uint8_t status, uint8_t progress_percent);

#endif // BOOTLOADER_COMMON_H
