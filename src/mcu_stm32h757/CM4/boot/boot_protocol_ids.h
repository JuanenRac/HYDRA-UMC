// =============================================================================
// HYDRA-UMC - firmware_base/boot/boot_protocol_ids.h
// Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
// GPL-3.0-or-later - see LICENSE
// =============================================================================
// The numbers of the update protocol that the three bootloaders (G474, H745 CM4, H745 CM7) share: the command
// offsets, the status codes, the verify-fail reasons and the metadata states. One copy here, the same bytes in each
// chip directory (tools/sync_firmware_base.py). Only pure #define lines: nothing is compiled from this header, so
// the binaries do not change.
//
// Not here on purpose: the axis, relay and poll offsets (+0x10, +0x11, +0x12, +0x13, +0x16, +0x17, +0x18). They
// belong to the chips that carry axes or the gateway, so each of those chips defines its own in its
// bootloader_common.h (the number is fixed by tools/can_ids.py and checked by tests/test_can_ids.py).
#ifndef BOOT_PROTOCOL_IDS_H
#define BOOT_PROTOCOL_IDS_H

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
#define OFS_HMAC_CHUNK           0x07 // the 64-byte Ed25519 signature of the image: DLC=8 x 8 frames in order, right after START_UPDATE (URTC bootloaders behind the relay still take an HMAC in 4)
#define OFS_QUERY_VERSION        0x08 // sent to either app or bootloader, whichever is running
#define OFS_VERSION_RESPONSE      0x09 // DLC=8: byte0(0=app,1=boot)+HardwareID(4)+verMajor(2)+verMinor(1)
#define OFS_BOOTLOADER_VERSION     0x0A // sent ONLY by the bootloader, alongside +0x09
#define OFS_QUERY_ERROR_COUNTERS   0x0B // sent to either app or bootloader
#define OFS_ERROR_COUNTERS_RESPONSE 0x0C // DLC=2: TEC + REC, read from the FDCAN peripheral's own PSR register
#define OFS_AUTHORIZE_DOWNGRADE     0x0D // DLC=4. The old constant-magic bypass: no longer accepted (anybody on the bus could send it).
                                             // A downgrade now needs OFS_DOWNGRADE_TOKEN (a signature), made for one image, one pair of versions and one session.
#define OFS_BACKUP_READ_REQUEST      0x0E // any DLC starts a main-slot readback
#define OFS_BACKUP_READ_RESPONSE      0x0F // first reply DLC=4 (total size), then DLC=8 raw bytes
// Found missing during implementation (not in the original docs/
// architecture.md section 4 table - added there alongside this file): the
// readback direction needs its OWN page-ack offset, distinct from +0x03's
// write-direction PAGE_ACK and from +0x0F's own data-response frames -
// URTC's own protocol has exactly this same 3-way split (0x7F3 write-ack /
// 0x7FE read-data / 0x7FF read-ack, CANBUS.TXT), which the original table
// here collapsed into 2 offsets by mistake. Master->bootloader, DLC=4,
// big-endian page index - same convention as +0x03, opposite direction.
#define OFS_BACKUP_READ_PAGE_ACK          0x14
// sent BY an external CAN master (never the application
// itself - see boot_decision.h's own header comment for why) once it has
// observed the current application behaving normally for a while. Any
// DLC. Resets FirmwareMetadata_t's own boot_attempts back to 0.
#define OFS_CONFIRM_HEALTHY                0x15
// The signed image header and the downgrade token (image_header.h, update_verify.h). The numbers follow tools/can_ids.py:
// 0x16-0x18 are the relay protocol, so these four take the free ones after them; each has ONE emitter.
#define OFS_IMAGE_HEADER                   0x19 // host -> bootloader, DLC=8 x 8 frames in order, after START_UPDATE: the 64-byte header of the image
#define OFS_DOWNGRADE_NONCE_REQUEST        0x1A // host -> bootloader, DLC=1: which half (0 or 1) of the 16-byte nonce of this session it wants
#define OFS_DOWNGRADE_TOKEN                0x1B // host -> bootloader, DLC=8 x 8 frames, optional: the Ed25519 signature that allows exactly one downgrade (see update_verify.h)
#define OFS_DOWNGRADE_NONCE_RESPONSE       0x1C // sent BY this bootloader, DLC=8: the half that was asked for
// Emergency stop state and slot scan (docs/ESTOP_CAN.md, docs/SLOT_SCAN.md). The stop itself is the identifier 0x001, outside the slot window.
#define OFS_ESTOP_STATE                    0x1D // sent BY the application of a controller board, DLC=4: latched or ready, epoch, last accepted sequence
#define OFS_SLOT_SCAN_PROBE                0x1E // host -> every board of the slot window, DLC=8: who is there? (slot_scan_node.h)
#define OFS_SLOT_SCAN_REPLY                0x1F // sent BY the bootloader or the application after a delay taken from its unique identifier, DLC=8

// F-RAM erase and self-test (docs/FRAM_ERASE.md, boot/fram_node.h). They need the window of 64 identifiers: the first 32 were all taken.
#define OFS_FRAM_ERASE_REQUEST             0x20 // host -> bootloader, DLC=8: erase the whole F-RAM (writes zeros and reads them back)
#define OFS_FRAM_ERASE_RESPONSE            0x21 // sent BY the bootloader, DLC=8: status and the bytes written
#define OFS_FRAM_SELFTEST_REQUEST          0x22 // host -> bootloader, DLC=8: write two patterns over a scratch area, read them back and put the old bytes back
#define OFS_FRAM_SELFTEST_RESPONSE         0x23 // sent BY the bootloader, DLC=8: status and the address that failed

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
#define VERIFY_FAIL_REASON_SIGNATURE    VERIFY_FAIL_REASON_HMAC // the same reason code: the signature of the image did not verify
#define VERIFY_FAIL_REASON_HARDWARE_ID 0x04
#define VERIFY_FAIL_REASON_ROLLBACK    0x05
#define VERIFY_FAIL_REASON_HEADER      0x06 // the signed header is missing, is not ours, or says something that is not true of what arrived
#define STATUS_ERROR          0xFF
// reported (via CAN_SendHeartbeat) once this bootloader has
// refused to jump to an app that reset back here BOOT_MAX_ATTEMPTS times in
// a row without ever being confirmed healthy - see boot_decision.h.
#define STATUS_ROLLBACK_SUSPECT 0x08

#define META_STATE_APP_VALID     1
#define META_STATE_COPY_PENDING  2

#endif // BOOT_PROTOCOL_IDS_H
