# =============================================================================
# HYDRA-UMC - Real SPI-OTA bootloader client (CM5 side)
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see repo root LICENSE
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

from . import image_header
from .protocol import (
    AUTHORIZE_DOWNGRADE_MAGIC,
    ENTER_BOOTLOADER_MAGIC,
    OFS_DATA,
    OFS_DOWNGRADE_NONCE_REQUEST,
    OFS_DOWNGRADE_NONCE_RESPONSE,
    OFS_DOWNGRADE_TOKEN,
    OFS_IMAGE_HEADER,
    OFS_END_UPDATE,
    OFS_CONFIRM_HEALTHY,
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
    poll_frame,
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
    except (OSError, ValueError):
        return VersionInfo(online=False)
    if response.frame_type != OFS_VERSION_RESPONSE or response.dlc < 8:
        return VersionInfo(online=False)
    is_bootloader = response.payload[0] == 0x01
    hardware_id = struct.unpack(">I", response.payload[1:5])[0]
    firmware_major = struct.unpack(">H", response.payload[5:7])[0]
    firmware_minor = response.payload[7]
    return VersionInfo(True, is_bootloader, hardware_id, firmware_major, firmware_minor)


def confirm_healthy(transport: SpiOtaTransport, target_tier: int, target_slot: int) -> bool:
    """Tells a bootloader that the application it started is healthy, which sets its count of unconfirmed starts back to 0. A board parked in the bootloader after repeated
    watchdog resets (heartbeat STATUS_ROLLBACK_SUSPECT) is let go by this once somebody has looked at it; the bootloader answers nothing, so a True only says the frame was
    sent. The bootloader only listens for it in its start-up window or while it is parked, so send it while the target is in the bootloader. False when the transport fails."""

    request = SpiOtaFrame(target_tier, target_slot, OFS_CONFIRM_HEALTHY, 1, b"\x00")
    try:
        transport.transceive(request)
    except (OSError, ValueError):
        return False
    return True


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
        version_patch: int = 0,
        allow_downgrade: bool = False,
        downgrade_from: tuple[int, int, int] | None = None,
        page_timeout_seconds: float = 3.0,
    ) -> Iterator[FlashProgress]:
        """Flash `firmware` to the board. The HYDRA-UMC bootloaders (image_header.is_signed_header_target) take a signed 64-byte header
        with the full version (major.minor.patch) and accept only an image strictly newer than the installed one; to go back to an older
        one `downgrade_from` must give the installed (major, minor, patch), and the board is asked for a nonce to build a token bound to
        this session, those two versions and this image. `allow_downgrade` is the old constant-magic frame: only the URTC bootloaders
        behind the relay still understand it, and for a HYDRA-UMC one it is refused here rather than sent."""
        pages_total = max(1, -(-len(firmware) // FLASH_PAGE_SIZE))  # ceil division
        signed = image_header.is_signed_header_target(hardware_id)
        version = (version_major, version_minor, version_patch)
        if signed and allow_downgrade and downgrade_from is None:
            yield FlashProgress("error", 0, pages_total, 0, "a downgrade of a HYDRA-UMC board needs the installed version (downgrade_from)")
            return
        if signed:
            try:
                header = image_header.build_header(hardware_id, version, firmware)
            except image_header.ImageHeaderError as error:
                yield FlashProgress("error", 0, pages_total, 0, str(error))
                return
            signature = image_header.tag_of(self._hmac_key, header, firmware)
        else:
            header = b""
            signature = hmac.new(self._hmac_key, firmware, hashlib.sha256).digest()

        yield FlashProgress("entering_bootloader", 0, pages_total, 0)
        self._send(target_tier, target_slot, OFS_ENTER_BOOTLOADER, ENTER_BOOTLOADER_MAGIC)
        time.sleep(0.8)  # matches URTC-FLASHER's own real post-reset settle delay

        if allow_downgrade and not signed:
            self._send(target_tier, target_slot, 0x0D, AUTHORIZE_DOWNGRADE_MAGIC)  # OFS_AUTHORIZE_DOWNGRADE (URTC bootloaders only)

        size_and_hwid = struct.pack(">II", len(firmware), hardware_id)
        self._send(target_tier, target_slot, OFS_START_UPDATE, size_and_hwid)

        for chunk_index in range(4):
            chunk = signature[chunk_index * 8 : (chunk_index + 1) * 8]
            self._send(target_tier, target_slot, OFS_HMAC_CHUNK, chunk)

        if signed:
            for chunk_index in range(image_header.HEADER_CHUNKS):
                self._send(target_tier, target_slot, OFS_IMAGE_HEADER, header[chunk_index * 8 : (chunk_index + 1) * 8])
            if downgrade_from is not None:
                token = self._downgrade_token(target_tier, target_slot, downgrade_from, version, signature)
                if token is None:
                    yield FlashProgress("error", 0, pages_total, 2, "the board did not give the nonce of this session")
                    return
                for chunk_index in range(image_header.TOKEN_CHUNKS):
                    self._send(target_tier, target_slot, OFS_DOWNGRADE_TOKEN, token[chunk_index * 8 : (chunk_index + 1) * 8])

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

    def _downgrade_token(self, target_tier: int, target_slot: int, installed: tuple[int, int, int], target: tuple[int, int, int], tag: bytes) -> bytes | None:
        """Asks the board for the nonce of this session (a half at a time: a frame carries 8 bytes) and builds the token from it."""
        nonce = b""
        for half in (0, 1):
            self._send(target_tier, target_slot, OFS_DOWNGRADE_NONCE_REQUEST, bytes([half]))
            answer = None
            for _ in range(20):
                response = self._transport.transceive(SpiOtaFrame(target_tier, target_slot, OFS_DOWNGRADE_NONCE_RESPONSE, 0))
                if response.frame_type == OFS_DOWNGRADE_NONCE_RESPONSE and response.dlc == 8:
                    answer = bytes(response.payload[:8])
                    break
            if answer is None:
                return None
            nonce += answer
        return image_header.downgrade_token(self._hmac_key, nonce, installed, target, tag)

    def _send_page_and_wait_ack(
        self, target_tier: int, target_slot: int, page_index: int, page: bytes, timeout_seconds: float
    ) -> bool:
        for offset in range(0, max(len(page), 1), 8):
            chunk = page[offset : offset + 8] or b""
            self._send(target_tier, target_slot, OFS_DATA, chunk)
        deadline = time.monotonic() + timeout_seconds
        while time.monotonic() < deadline:
            response = self._transport.transceive(poll_frame(target_tier, target_slot, OFS_PAGE_ACK))
            if response.frame_type == OFS_PAGE_ACK and response.dlc >= 4:
                acked_index = struct.unpack(">I", response.payload[:4])[0]
                if acked_index == page_index:
                    return True
        return False

    def _wait_for_status(self, target_tier: int, target_slot: int, timeout_seconds: float) -> int:
        deadline = time.monotonic() + timeout_seconds
        while time.monotonic() < deadline:
            response = self._transport.transceive(poll_frame(target_tier, target_slot, OFS_STATUS))
            if response.frame_type in (OFS_STATUS, OFS_HEARTBEAT) and response.dlc >= 1:
                status = response.payload[0]
                if status in (STATUS_VERIFY_OK, STATUS_VERIFY_FAIL, STATUS_ERROR):
                    return status
        return STATUS_ERROR

    def _send(self, target_tier: int, target_slot: int, frame_type: int, payload: bytes) -> None:
        self._transport.transceive(SpiOtaFrame(target_tier, target_slot, frame_type, len(payload), payload))
