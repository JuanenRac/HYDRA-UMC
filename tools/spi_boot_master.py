#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/spi_boot_master.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Host model of the bootloader as SPI master: it receives a signed image from the host (the Jetson) and verifies it with the same chain of trust as over CAN.

The board of production is the SPI master towards the Jetson Orin NX (docs/SPI_BOOT_MASTER.md). In this mode the bootloader asks, the host answers, and nothing is
executed or marked valid until the whole image has been verified by `image_header.verify_image`, the very function that decides over CAN: the Ed25519 signature
first, then the CRC, the hardware identifier, the link address, the vectors, the stack pointer and the anti-rollback rule with its signed downgrade token. This model
adds only checks that refuse earlier (a target or a minimum bootloader that does not fit is refused before a byte is fetched) and never skips one.

The mode is OFF by default (`BootConfig.spi_master_enabled`, `HYDRA_BOOT_SPI_MASTER` in the firmware build): with it off the model does not touch the link and the
update goes on over CAN as before. The READY line is the one the host raises when it has something to ask for: the master clocks, the slave warns.

Frames (little endian, 6 + payload + 4 bytes), one per transaction, the answer on the next transaction once READY is high:

    0x53 'S' | type | sequence u16 | length u16 | payload (at most 260: a page of 256 bytes and its offset) | CRC-32 of everything before it

    types master -> host: 0x01 HELLO, 0x02 GET_PAGE, 0x03 GET_TOKEN, 0x04 DONE
    types host -> master: 0x81 OFFER, 0x82 PAGE, 0x83 TOKEN, 0x8F REFUSE

Evidence: simulator (this model and tests/test_spi_boot_master.py). No board and no Jetson has run it."""
from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass, field

import image_header as ih

SYNC = 0x53
MAX_PAGE = 256  # bytes of image per page
MAX_PAYLOAD = MAX_PAGE + 4  # a page frame carries its offset in front
FRAME_OVERHEAD = 6 + 4
T_HELLO, T_GET_PAGE, T_GET_TOKEN, T_DONE = 0x01, 0x02, 0x03, 0x04
T_OFFER, T_PAGE, T_TOKEN, T_REFUSE = 0x81, 0x82, 0x83, 0x8F
MASTER_TYPES = {T_HELLO, T_GET_PAGE, T_GET_TOKEN, T_DONE}
HOST_TYPES = {T_OFFER, T_PAGE, T_TOKEN, T_REFUSE}
MAX_IMAGE_BYTES = 384 * 1024 + ih.HEADER_SIZE + ih.SIGNATURE_SIZE  # the application slot of a core (docs/H757_MEMORY_MAP.md)
MAX_RETRIES = 3

# result of an attempt
DISABLED, ACCEPTED, REFUSED, FAILED = "disabled", "accepted", "refused", "failed"


class FrameError(ValueError):
    """A frame that cannot be used: the reason is in the message."""


def encode(kind: int, sequence: int, payload: bytes = b"") -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise FrameError("the payload is longer than a frame can carry")
    body = struct.pack("<BBHH", SYNC, kind, sequence & 0xFFFF, len(payload)) + payload
    return body + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)


def decode(frame: bytes, allowed: set[int]) -> tuple[int, int, bytes]:
    """(type, sequence, payload) of a frame, or a FrameError. The CRC is checked before anything else is believed."""
    if len(frame) < FRAME_OVERHEAD:
        raise FrameError("shorter than the smallest frame")
    if struct.unpack_from("<I", frame, len(frame) - 4)[0] != zlib.crc32(frame[:-4]) & 0xFFFFFFFF:
        raise FrameError("the CRC does not match")
    sync, kind, sequence, length = struct.unpack_from("<BBHH", frame, 0)
    if sync != SYNC:
        raise FrameError("no sync byte")
    if length > MAX_PAYLOAD or len(frame) != FRAME_OVERHEAD + length:
        raise FrameError("the length does not match the frame")
    if kind not in allowed:
        raise FrameError(f"unexpected frame type 0x{kind:02X}")
    return kind, sequence, frame[6 : 6 + length]


@dataclass
class BootConfig:
    """What the bootloader is told by its configuration; the defaults are the conservative ones."""

    spi_master_enabled: bool = False  # off: the update stays on CAN
    hw_id: int = 0x48374337  # "H7C7": the application core of the kinematic brain
    link_address: int = 0x08040000
    bootloader_version: tuple[int, int, int] = (0, 1, 8)
    installed_version: tuple[int, int, int] = (0, 1, 7)
    ram_range: tuple[int, int] = (0x24000000, 0x24080000)
    public_key: bytes = b""
    ready_timeout_polls: int = 8
    target: str = "H757-CM7"


@dataclass
class StagingSlot:
    """The slot where the image waits: nothing in it is valid until the whole image verified, and it is never run from here."""

    data: bytearray = field(default_factory=bytearray)
    valid: bool = False
    version: tuple[int, int, int] | None = None

    def reset(self) -> None:
        self.data = bytearray()
        self.valid = False
        self.version = None


@dataclass
class Result:
    status: str
    reason: str = ""
    pages: int = 0
    frames: int = 0


class SpiHost:
    """The Jetson side: serves an image blob page by page. It can be made to misbehave for the tests."""

    def __init__(self, blob: bytes, *, version: tuple[int, int, int], min_bootloader: tuple[int, int, int] | None = None, target: str | None = None, token: bytes = b""):
        self.blob = blob
        self.version = version
        self.min_bootloader = min_bootloader
        self.target = target
        self.token = token
        self.ready = True
        self.corrupt_page: int | None = None  # corrupt the frame of this page the first `corrupt_times` times
        self.corrupt_times = 0
        self.drop_ready_after: int | None = None  # the READY line stays low after this many answers
        self.wrong_sequence = False
        self.oversize_offer: int | None = None
        self.requests: list[tuple[int, bytes]] = []
        self.answers = 0
        self._corrupted = 0

    def is_ready(self) -> bool:
        return self.ready and (self.drop_ready_after is None or self.answers < self.drop_ready_after)

    def transaction(self, frame: bytes) -> bytes:
        kind, sequence, payload = decode(frame, MASTER_TYPES)
        self.requests.append((kind, payload))
        self.answers += 1
        answer_sequence = sequence + 1 if self.wrong_sequence else sequence
        if kind == T_HELLO:
            size = len(self.blob) if self.oversize_offer is None else self.oversize_offer
            offer = struct.pack("<IHHH", size, *self.version) + struct.pack("<HHH", *(self.min_bootloader or (0, 0, 0)))
            offer += (self.target or "").encode("ascii")[:16].ljust(16, b"\x00")
            return encode(T_OFFER, answer_sequence, offer)
        if kind == T_GET_PAGE:
            offset, length = struct.unpack("<II", payload)
            page = self.blob[offset : offset + length]
            out = encode(T_PAGE, answer_sequence, struct.pack("<I", offset) + page)
            if self.corrupt_page is not None and offset // MAX_PAGE == self.corrupt_page and self._corrupted < self.corrupt_times:
                self._corrupted += 1
                return out[:20] + bytes([out[20] ^ 0x40]) + out[21:]
            return out
        if kind == T_GET_TOKEN:
            return encode(T_TOKEN, answer_sequence, self.token)
        return encode(T_REFUSE, answer_sequence, b"done")


class SpiMasterBootloader:
    """The bootloader side. `receive` runs one whole attempt; it returns a Result and leaves the staging slot valid only on ACCEPTED."""

    def __init__(self, config: BootConfig, slot: StagingSlot | None = None):
        self.config = config
        self.slot = slot if slot is not None else StagingSlot()
        self.sequence = 0
        self.nonce = b"\x00" * ih.NONCE_SIZE
        self.frames = 0

    def _exchange(self, host: SpiHost, kind: int, payload: bytes) -> tuple[int, bytes]:
        """One request and its answer, with the retries a damaged frame deserves. Raises FrameError when the link cannot be used."""
        last: FrameError | None = None
        for _ in range(MAX_RETRIES):
            polls = 0
            self.sequence = (self.sequence + 1) & 0xFFFF
            expected = self.sequence
            self.frames += 1
            try:
                # the request goes out; the answer is read once READY is high
                answer_frame = host.transaction(encode(kind, expected, payload))
            except FrameError as error:
                last = error
                continue
            while not host.is_ready():
                polls += 1
                if polls > self.config.ready_timeout_polls:
                    raise FrameError("the host never raised READY")
            try:
                answer, sequence, body = decode(answer_frame, HOST_TYPES)
            except FrameError as error:
                last = error
                continue
            if sequence != expected:
                last = FrameError("the answer belongs to another request")
                continue
            return answer, body
        raise FrameError(f"the link failed {MAX_RETRIES} times: {last}")

    def receive(self, host: SpiHost, token_wanted: bool = False) -> Result:
        cfg = self.config
        if not cfg.spi_master_enabled:
            return Result(DISABLED, "the SPI master update is off; the update goes on over CAN")
        self.slot.reset()
        self.nonce = bytes(range(ih.NONCE_SIZE))  # a real bootloader draws it from its random source for each attempt
        try:
            kind, body = self._exchange(host, T_HELLO, struct.pack("<IIHHH", cfg.hw_id, cfg.link_address, *cfg.installed_version) + self.nonce + struct.pack("<HHH", *cfg.bootloader_version))
            if kind == T_REFUSE:
                return Result(REFUSED, "the host has nothing to offer", frames=self.frames)
            if kind != T_OFFER or len(body) != 32:
                return Result(FAILED, "the offer is not an offer", frames=self.frames)
            size = struct.unpack_from("<I", body, 0)[0]
            target = body[16:32].rstrip(b"\x00").decode("ascii", "replace")
            # checks that refuse before a byte is fetched; none of them replaces the verification below
            if size < ih.HEADER_SIZE + ih.SIGNATURE_SIZE or size > MAX_IMAGE_BYTES:
                return Result(REFUSED, "the size offered does not fit the application slot", frames=self.frames)
            if target and target != cfg.target:
                return Result(REFUSED, f"the image is for {target}, this board is {cfg.target}", frames=self.frames)
            offered_min = self._unpack_min(body)
            if offered_min is not None and offered_min > cfg.bootloader_version:
                return Result(REFUSED, "this bootloader is older than the image requires", frames=self.frames)
            blob = bytearray()
            pages = 0
            while len(blob) < size:
                length = min(MAX_PAGE, size - len(blob))
                kind, body = self._exchange(host, T_GET_PAGE, struct.pack("<II", len(blob), length))
                if kind != T_PAGE or len(body) != 4 + length or struct.unpack_from("<I", body, 0)[0] != len(blob):
                    return Result(FAILED, "a page did not come as asked", pages=pages, frames=self.frames)
                blob += body[4:]
                pages += 1
            self.slot.data = blob
            token = b""
            if token_wanted:
                kind, body = self._exchange(host, T_GET_TOKEN, b"")
                token = body if kind == T_TOKEN else b""
            try:
                header = ih.verify_image(
                    bytes(blob), cfg.public_key, hw_id=cfg.hw_id, link_address=cfg.link_address, installed_version=cfg.installed_version,
                    ram_range=cfg.ram_range, nonce=self.nonce, token=token,
                )
            except ih.ImageError as error:
                self.slot.reset()
                return Result(REFUSED, str(error), pages=pages, frames=self.frames)
            self.slot.valid = True
            self.slot.version = header.version
            try:
                self._exchange(host, T_DONE, b"")
            except FrameError:
                pass  # the image is verified and staged; the host not hearing the end does not undo that
            return Result(ACCEPTED, pages=pages, frames=self.frames)
        except FrameError as error:
            self.slot.reset()
            return Result(FAILED, str(error), frames=self.frames)

    @staticmethod
    def _unpack_min(body: bytes) -> tuple[int, int, int] | None:
        major, minor, patch = struct.unpack_from("<HHH", body, 10)
        return None if (major, minor, patch) == (0, 0, 0) else (major, minor, patch)
