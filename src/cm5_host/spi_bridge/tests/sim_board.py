# =============================================================================
# HYDRA-UMC - Simulated bootloader with an A/B flash and injectable faults (host only)
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see repo root LICENSE
# =============================================================================
"""A host model of a bootloader behind the SPI-OTA bridge, for fault tests: nothing here runs on a board (evidence: simulator).

The model keeps two flash slots. An update is staged in the inactive slot page by page and the active slot changes only when the whole image
verified (size, CRC-32 of what was received). So a power cut, a flash error or a bad frame at any point leaves the active slot as it was.
`SimulatedBoard.transceive` has the interface of a SpiOtaTransport. `faults` is a list of `Fault` objects the transport checks before it
answers; each one fires on a chosen frame count and does one thing (cut the power, flip a bit of a frame, drop or repeat the answer,
truncate a data chunk, raise an ECC error on a page)."""

import struct
import zlib
from dataclasses import dataclass

from spi_bridge.protocol import (
    OFS_DATA,
    OFS_END_UPDATE,
    OFS_ENTER_BOOTLOADER,
    OFS_HMAC_CHUNK,
    OFS_PAGE_ACK,
    OFS_POLL_STATUS,
    OFS_START_UPDATE,
    OFS_STATUS,
    STATUS_ERROR,
    STATUS_VERIFY_FAIL,
    STATUS_VERIFY_OK,
    SpiOtaFrame,
    polled_frame_type,
)

PAGE = 2048


class PowerCut(OSError):
    """The board lost power: the SPI transaction never completed."""


@dataclass
class Fault:
    """One injected fault. `kind`: power_cut, bit_flip (flips a bit of the payload of a frame the board receives), drop_answer, duplicate_answer,
    ecc_error (the flash page being written reports an uncorrectable error). It fires once, on the frame whose 0-based count is `at_frame`."""

    kind: str
    at_frame: int
    fired: bool = False


class SimulatedBoard:
    def __init__(self, *, old_image: bytes = b"OLD" * 100, faults: list[Fault] | None = None, hardware_id: int = 0x0303CC01):
        self.hardware_id = hardware_id
        self.slots = {"A": bytes(old_image), "B": b""}
        self.active = "A"
        self.faults = list(faults or [])
        self.frame_count = 0
        self.powered = True
        self._reset_session()
        self._last_answer: SpiOtaFrame | None = None

    def _reset_session(self) -> None:
        self.expected_size = 0
        self.received = bytearray()
        self.pages_done = 0
        self.status = 0
        self.signature = bytearray()
        self.session_open = False

    def power_up(self) -> None:
        """The board restarts: a half-written staging slot is dropped, the active slot is what it was."""
        self.powered = True
        self._reset_session()
        self.slots["B" if self.active == "A" else "A"] = b""

    @property
    def active_image(self) -> bytes:
        return self.slots[self.active]

    def transceive(self, frame: SpiOtaFrame) -> SpiOtaFrame:
        if not self.powered:
            raise PowerCut("no response (no power)")
        index = self.frame_count
        self.frame_count += 1
        if self._fire_at(index, "power_cut"):
            self.powered = False
            raise PowerCut("power lost during the transfer")
        if self._fire_at(index, "bit_flip") and frame.payload:
            damaged = bytearray(frame.payload)
            damaged[0] ^= 0x01
            frame = SpiOtaFrame(frame.target_tier, frame.target_slot, frame.frame_type, frame.dlc, bytes(damaged))
        answer = self._handle(frame, index)
        if self._fire_at(index, "drop_answer"):
            raise OSError("no response (frame lost)")
        if self._fire_at(index, "duplicate_answer"):
            previous = self._last_answer
            self._last_answer = answer
            return previous if previous is not None else answer
        self._last_answer = answer
        return answer

    def _fire_at(self, index: int, kind: str) -> bool:
        for fault in self.faults:
            if fault.kind == kind and not fault.fired and fault.at_frame == index:
                fault.fired = True
                return True
        return False

    def _handle(self, frame: SpiOtaFrame, index: int) -> SpiOtaFrame:
        tier, slot = frame.target_tier, frame.target_slot
        none = SpiOtaFrame(tier, slot, 0xFF, 0)
        wanted = polled_frame_type(frame)
        kind = wanted if wanted is not None else frame.frame_type
        if frame.frame_type == OFS_POLL_STATUS and wanted is not None:
            frame = SpiOtaFrame(tier, slot, wanted, 0)
        if frame.frame_type == OFS_ENTER_BOOTLOADER and wanted is None:
            self._reset_session()
            self.session_open = True
            return none
        if frame.frame_type == OFS_START_UPDATE and wanted is None:
            self.expected_size = struct.unpack(">II", frame.payload[:8])[0]
            self.received = bytearray()
            self.pages_done = 0
            self.status = 0
            return none
        if frame.frame_type == OFS_HMAC_CHUNK and wanted is None:
            self.signature += frame.payload[: frame.dlc]
            return none
        if frame.frame_type == OFS_DATA and wanted is None:
            chunk = frame.payload[: frame.dlc]
            self.received += chunk
            if len(self.received) >= (self.pages_done + 1) * PAGE or (frame.dlc < 8 and len(self.received) % PAGE == 0) or len(self.received) >= self.expected_size:
                if self._fire_at(index, "ecc_error"):
                    self.status = STATUS_ERROR
                else:
                    self.pages_done += 1
            return none
        if kind == OFS_PAGE_ACK:
            if self.status == STATUS_ERROR or self.pages_done == 0:
                return SpiOtaFrame(tier, slot, 0xFF, 0)
            return SpiOtaFrame(tier, slot, OFS_PAGE_ACK, 4, struct.pack(">I", self.pages_done - 1))
        if frame.frame_type == OFS_END_UPDATE and wanted is None:
            crc, _major, _minor = struct.unpack(">IHH", frame.payload[:8])
            if self.status == STATUS_ERROR:
                pass
            elif len(self.received) == self.expected_size and (zlib.crc32(bytes(self.received)) & 0xFFFFFFFF) == crc:
                inactive = "B" if self.active == "A" else "A"
                self.slots[inactive] = bytes(self.received)
                self.active = inactive  # the switch happens once, after the whole image verified
                self.status = STATUS_VERIFY_OK
            else:
                self.status = STATUS_VERIFY_FAIL
            return none
        if kind == OFS_STATUS:
            return SpiOtaFrame(tier, slot, OFS_STATUS, 1, bytes([self.status or STATUS_ERROR]))
        return none

    def wait_data_ready(self, timeout_seconds: float) -> bool:
        return True

    def close(self) -> None:
        pass
