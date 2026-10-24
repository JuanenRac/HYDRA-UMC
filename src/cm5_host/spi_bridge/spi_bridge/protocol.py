# =============================================================================
# HYDRA-UMC - CM5 <-> STM32H745 SPI-OTA protocol (real frame format)
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see repo root LICENSE
# =============================================================================
"""The real, exact 128-byte SpiOtaFrame_t wire format and OFS_*/STATUS_*
constants - a byte-for-byte match of
src/mcu_stm32h745/CM4/boot/bootloader_common.h's own real C struct and
#defines, not a re-derived or approximate shape.

    typedef struct {
        uint8_t target_tier; // SPI_TARGET_*
        uint8_t target_slot; // 0-7, meaningful only when target_tier == SPI_TARGET_STACKA
        uint8_t frame_type;  // OFS_*
        uint8_t dlc;         // 0-8
        uint8_t payload[8];
        uint8_t _reserved[SPI_FRAME_SIZE - 12];
    } SpiOtaFrame_t; // 128 bytes exactly

This module owns only the frame shape and its constants - CRC32/HMAC/retry
state machine logic lives in bootloader_client.py, real SPI/GPIO I/O lives
in transport.py. Kept separate so each is independently testable.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

SPI_FRAME_SIZE = 128

SPI_TARGET_SELF = 0
SPI_TARGET_CM7 = 1
SPI_TARGET_STACKA = 2

# Same offset space as every bootloader in this project (docs/architecture.md
# section 4) - used here as the SPI1 frame_type byte.
OFS_ENTER_BOOTLOADER = 0x00
OFS_START_UPDATE = 0x01
OFS_DATA = 0x02
OFS_PAGE_ACK = 0x03
OFS_END_UPDATE = 0x04
OFS_STATUS = 0x05
OFS_HEARTBEAT = 0x06
OFS_HMAC_CHUNK = 0x07
OFS_QUERY_VERSION = 0x08
OFS_VERSION_RESPONSE = 0x09
OFS_BOOTLOADER_VERSION = 0x0A
OFS_QUERY_ERROR_COUNTERS = 0x0B
OFS_ERROR_COUNTERS_RESPONSE = 0x0C
OFS_AUTHORIZE_DOWNGRADE = 0x0D
OFS_BACKUP_READ_REQUEST = 0x0E
OFS_BACKUP_READ_RESPONSE = 0x0F
OFS_BACKUP_READ_PAGE_ACK = 0x14
OFS_CONFIRM_HEALTHY = 0x15
# The signed image header and the downgrade token of the HYDRA-UMC bootloaders (tools/image_header.py, image_header.py here)
OFS_IMAGE_HEADER = 0x19
OFS_DOWNGRADE_NONCE_REQUEST = 0x1A
OFS_DOWNGRADE_TOKEN = 0x1B
OFS_DOWNGRADE_NONCE_RESPONSE = 0x1C
OFS_RELAY_SEND = 0x12
OFS_RELAY_RECV = 0x13
# One emitter per identifier (tools/can_ids.py): the request of the kinematic brain and the answer of the controller board no longer share an
# identifier, and the host no longer polls with the identifiers the bootloader transmits (PAGE_ACK 0x03, STATUS 0x05).
OFS_AXIS_STATUS = 0x10
OFS_AXIS_STATUS_RESPONSE = 0x16
OFS_POLL_STATUS = 0x17  # host -> gateway, DLC 1: the frame type (PAGE_ACK or STATUS) to wait for; nothing is put on the CAN bus
OFS_RELAY_RECV_RESPONSE = 0x18

STATUS_LISTENING = 0x01
STATUS_ERASING = 0x02
STATUS_RECEIVING = 0x03
STATUS_VERIFYING = 0x06
STATUS_COPYING = 0x07
STATUS_VERIFY_OK = 0x04
STATUS_VERIFY_FAIL = 0x05
STATUS_ROLLBACK_SUSPECT = 0x08
STATUS_ERROR = 0xFF

VERIFY_FAIL_REASON_INCOMPLETE = 0x01
VERIFY_FAIL_REASON_CRC32 = 0x02
VERIFY_FAIL_REASON_HMAC = 0x03
VERIFY_FAIL_REASON_HARDWARE_ID = 0x04
VERIFY_FAIL_REASON_ROLLBACK = 0x05
VERIFY_FAIL_REASON_HEADER = 0x06

# Real magic payload of the frame type that gates a privileged action - bootloader_main.c ignores ENTER_BOOTLOADER without this exact payload,
# matching URTC's own proven CAN bootloader convention (see docs/CANBUS.TXT).
ENTER_BOOTLOADER_MAGIC = bytes((0xB0, 0x07, 0x1D, 0x5A))
# The four constant bytes that switched the anti-rollback off in the bootloaders of URTC. The HYDRA-UMC bootloaders no longer accept them (a
# downgrade needs a token bound to the session, the versions and the image); they are still what the URTC ones behind the relay understand.
AUTHORIZE_DOWNGRADE_MAGIC = bytes((0xD0, 0x9E, 0x12, 0xAD))

_STRUCT_FORMAT = f"<BBBB8s{SPI_FRAME_SIZE - 12}s"


@dataclass(frozen=True)
class SpiOtaFrame:
    """Python mirror of the real, exact C SpiOtaFrame_t struct above."""

    target_tier: int
    target_slot: int
    frame_type: int
    dlc: int
    payload: bytes = field(default=b"")

    def __post_init__(self) -> None:
        if not (0 <= self.target_tier <= 255):
            raise ValueError("target_tier must fit a uint8_t")
        if not (0 <= self.target_slot <= 255):
            raise ValueError("target_slot must fit a uint8_t")
        if not (0 <= self.frame_type <= 255):
            raise ValueError("frame_type must fit a uint8_t")
        if not (0 <= self.dlc <= 8):
            raise ValueError("dlc must be 0-8, matching the real 8-byte payload field")
        if len(self.payload) > 8:
            raise ValueError("payload must fit the real 8-byte SpiOtaFrame_t.payload field")

    def to_bytes(self) -> bytes:
        """Pack into the real, exact 128-byte wire format."""

        padded_payload = self.payload.ljust(8, b"\x00")
        reserved = b"\xff" * (SPI_FRAME_SIZE - 12)  # matches the real firmware's own 0xFF padding convention
        # _STRUCT_FORMAT is a fixed layout (4 uint8_t + 8s + 116s = 128 bytes)
        # - struct.pack() always returns exactly SPI_FRAME_SIZE bytes here,
        # not something that needs a runtime check.
        return struct.pack(_STRUCT_FORMAT, self.target_tier, self.target_slot, self.frame_type, self.dlc, padded_payload, reserved)

    @classmethod
    def from_bytes(cls, raw: bytes) -> "SpiOtaFrame":
        if len(raw) != SPI_FRAME_SIZE:
            raise ValueError(f"SPI frame must be exactly {SPI_FRAME_SIZE} bytes, got {len(raw)}")
        target_tier, target_slot, frame_type, dlc, payload, _reserved = struct.unpack(_STRUCT_FORMAT, raw)
        return cls(target_tier, target_slot, frame_type, dlc, payload[: min(dlc, 8)])


def poll_frame(target_tier: int, target_slot: int, wanted_frame_type: int) -> SpiOtaFrame:
    """The frame that asks the gateway for the next PAGE_ACK or STATUS of a board without putting anything on the CAN bus."""

    return SpiOtaFrame(target_tier, target_slot, OFS_POLL_STATUS, 1, bytes((wanted_frame_type,)))


def polled_frame_type(frame: SpiOtaFrame) -> int | None:
    """The frame type a POLL_STATUS frame waits for, or None when the frame is not a poll."""

    if frame.frame_type != OFS_POLL_STATUS or frame.dlc < 1:
        return None
    return frame.payload[0]
