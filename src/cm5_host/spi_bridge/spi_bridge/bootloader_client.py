# =============================================================================
# HYDRA-UMC - Real SPI-OTA bootloader client (CM5 side)
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0 - see repo root LICENSE
# =============================================================================
"""Real SPI-OTA state machine: version query, and the full flash cycle.

Ports the real, already-proven state-machine reasoning from the sibling
URTC-FLASHER tool's own `flasher_protocol.py` (byte-for-byte CRC32/HMAC-
SHA256-verified against that project's real bootloader C, per its own
README) - adapted from CAN framing (a target CAN ID per step) to this
project's real SPI framing (a `frame_type` byte inside a fixed-target
`SpiOtaFrame`, per bootloader_common.h) - not a re-derived or guessed
protocol.

Every method here takes an already-open `SpiOtaTransport` (see
transport.py) rather than opening one itself, so this whole module is
unit-testable against an in-memory fake transport - no real SPI/GPIO/
STM32H745 hardware required to prove the state machine is correct.
"""

from __future__ import annotations

import hashlib
import hmac
import struct
import time
import zlib
from dataclasses import dataclass
from typing import Iterator

from .protocol import (
    AUTHORIZE_DOWNGRADE_MAGIC,
    ENTER_BOOTLOADER_MAGIC,
    OFS_DATA,
    OFS_END_UPDATE,
    OFS_ENTER_BOOTLOADER,
    OFS_HEARTBEAT,
    OFS_HMAC_CHUNK,
    OFS_PAGE_ACK,
    OFS_QUERY_VERSION,
    OFS_START_UPDATE,
    OFS_STATUS,
    OFS_VERSION_RESPONSE,
    STATUS_ERROR,
    STATUS_VERIFY_FAIL,
    STATUS_VERIFY_OK,
    SpiOtaFrame,
)
from .transport import SpiOtaTransport

FLASH_PAGE_SIZE = 2048  # matches OFS_DATA's real 2 KB logical transfer-chunk size


@dataclass(frozen=True)
class VersionInfo:
    online: bool
    is_bootloader: bool = False
    hardware_id: int = 0
    firmware_major: int = 0
    firmware_minor: int = 0


def query_version(transport: SpiOtaTransport, target_tier: int, target_slot: int) -> VersionInfo:
    """Real VERSION_QUERY/VERSION_RESPONSE round trip - mirrors URTC-FLASHER's
    own query_version(), adapted to SPI framing."""

    request = SpiOtaFrame(target_tier, target_slot, OFS_QUERY_VERSION, 1, b"\x00")
    try:
        response = transport.transceive(request)
    except OSError:
        return VersionInfo(online=False)
    if response.frame_type != OFS_VERSION_RESPONSE or response.dlc < 8:
        return VersionInfo(online=False)
    is_bootloader = response.payload[0] == 0x01
    hardware_id = struct.unpack(">I", response.payload[1:5])[0]
    firmware_major = struct.unpack(">H", response.payload[5:7])[0]
    firmware_minor = response.payload[7]
    return VersionInfo(True, is_bootloader, hardware_id, firmware_major, firmware_minor)


@dataclass(frozen=True)
class FlashProgress:
    phase: str  # "entering_bootloader" | "transferring" | "verifying" | "done" | "error"
    pages_sent: int
    pages_total: int
    percent: int
    error: str | None = None


class SpiOtaFlasher:
    """Real ENTER_BOOTLOADER -> START_UPDATE -> HMAC_CHUNK x4 -> DATA (page
    by page) -> END_UPDATE -> STATUS verify cycle. Mirrors the sibling
    URTC-FLASHER tool's own `URTCFlasher` state machine (see this module's
    own docstring), adapted to SPI framing."""

    def __init__(self, transport: SpiOtaTransport, hmac_key: bytes):
        self._transport = transport
        self._hmac_key = hmac_key

    def flash(
        self,
        target_tier: int,
        target_slot: int,
        hardware_id: int,
        firmware: bytes,
        version_major: int,
        version_minor: int,
        *,
        allow_downgrade: bool = False,
        page_timeout_seconds: float = 3.0,
    ) -> Iterator[FlashProgress]:
        pages_total = max(1, -(-len(firmware) // FLASH_PAGE_SIZE))  # ceil division

        yield FlashProgress("entering_bootloader", 0, pages_total, 0)
        self._send(target_tier, target_slot, OFS_ENTER_BOOTLOADER, ENTER_BOOTLOADER_MAGIC)
        time.sleep(0.8)  # matches URTC-FLASHER's own real post-reset settle delay

        if allow_downgrade:
            self._send(target_tier, target_slot, 0x0D, AUTHORIZE_DOWNGRADE_MAGIC)  # OFS_AUTHORIZE_DOWNGRADE

        size_and_hwid = struct.pack(">II", len(firmware), hardware_id)
        self._send(target_tier, target_slot, OFS_START_UPDATE, size_and_hwid)

        signature = hmac.new(self._hmac_key, firmware, hashlib.sha256).digest()
        for chunk_index in range(4):
            chunk = signature[chunk_index * 8 : (chunk_index + 1) * 8]
            self._send(target_tier, target_slot, OFS_HMAC_CHUNK, chunk)

        for page_index in range(pages_total):
            page = firmware[page_index * FLASH_PAGE_SIZE : (page_index + 1) * FLASH_PAGE_SIZE]
            if not self._send_page_and_wait_ack(target_tier, target_slot, page_index, page, page_timeout_seconds):
                yield FlashProgress("error", page_index, pages_total, 5 + round(page_index / pages_total * 80), "page ack timeout")
                return
            percent = 5 + round((page_index + 1) / pages_total * 80)
            yield FlashProgress("transferring", page_index + 1, pages_total, percent)

        crc32 = zlib.crc32(firmware) & 0xFFFFFFFF
        end_payload = struct.pack(">IHH", crc32, version_major, version_minor)
        self._send(target_tier, target_slot, OFS_END_UPDATE, end_payload)

        yield FlashProgress("verifying", pages_total, pages_total, 90)
        outcome = self._wait_for_status(target_tier, target_slot, timeout_seconds=10.0)
        if outcome != STATUS_VERIFY_OK:
            reason = "verify failed" if outcome == STATUS_VERIFY_FAIL else "no verify response"
            yield FlashProgress("error", pages_total, pages_total, 90, reason)
            return

        yield FlashProgress("done", pages_total, pages_total, 100)

    def _send_page_and_wait_ack(
        self, target_tier: int, target_slot: int, page_index: int, page: bytes, timeout_seconds: float
    ) -> bool:
        for offset in range(0, max(len(page), 1), 8):
            chunk = page[offset : offset + 8] or b""
            self._send(target_tier, target_slot, OFS_DATA, chunk)
        deadline = time.monotonic() + timeout_seconds
        while time.monotonic() < deadline:
            response = self._transport.transceive(SpiOtaFrame(target_tier, target_slot, OFS_PAGE_ACK, 0))
            if response.frame_type == OFS_PAGE_ACK and response.dlc >= 4:
                acked_index = struct.unpack(">I", response.payload[:4])[0]
                if acked_index == page_index:
                    return True
        return False

    def _wait_for_status(self, target_tier: int, target_slot: int, timeout_seconds: float) -> int:
        deadline = time.monotonic() + timeout_seconds
        while time.monotonic() < deadline:
            response = self._transport.transceive(SpiOtaFrame(target_tier, target_slot, OFS_STATUS, 0))
            if response.frame_type in (OFS_STATUS, OFS_HEARTBEAT) and response.dlc >= 1:
                status = response.payload[0]
                if status in (STATUS_VERIFY_OK, STATUS_VERIFY_FAIL, STATUS_ERROR):
                    return status
        return STATUS_ERROR

    def _send(self, target_tier: int, target_slot: int, frame_type: int, payload: bytes) -> None:
        self._transport.transceive(SpiOtaFrame(target_tier, target_slot, frame_type, len(payload), payload))
