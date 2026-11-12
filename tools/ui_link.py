#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/ui_link.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Reference of the exchange between the H757 (SPI master) and the Jetson for the interface of the display: the status that is the heartbeat of the view-model and the still
picture of a camera (`src/ui_h757/spec/jetson_link.json`).

The master side (`LinkMaster`) polls, reassembles the picture and turns what it hears into events of the view-model (`tools/ui_model.py`): a valid status is a heartbeat, a
damaged answer or none is an SPI error. The host side is a double (`FakeJetson`) whose behaviour can be set: answer, stay silent, damage a frame, hang in the middle of a picture.
Everything is driven by the time a caller passes in, so a run is deterministic. The framing and the CRC are those of `tools/spi_boot_master.py`, which this file only reads.
Evidence: simulator; nothing here has run on a Jetson or a board."""
from __future__ import annotations

import json
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import spi_boot_master as boot  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
SPEC_PATH = ROOT / "src" / "ui_h757" / "spec" / "jetson_link.json"
CONFIG = json.loads(SPEC_PATH.read_text(encoding="utf-8"))
LIMITS = CONFIG["limits"]
FRAMES = {f["name"]: f for f in CONFIG["frames"]}
T_STATUS_REQ, T_SNAPSHOT_REQ = FRAMES["STATUS_REQ"]["type"], FRAMES["SNAPSHOT_REQ"]["type"]
T_STATUS, T_SNAPSHOT, T_SNAPSHOT_NONE = FRAMES["STATUS"]["type"], FRAMES["SNAPSHOT"]["type"], FRAMES["SNAPSHOT_NONE"]["type"]
MASTER_TYPES = {T_STATUS_REQ, T_SNAPSHOT_REQ}
HOST_TYPES = {T_STATUS, T_SNAPSHOT, T_SNAPSHOT_NONE}
STATUS_FORMAT = "<BBBhBI"
SNAPSHOT_HEADER = "<HII"
SNAPSHOT_REQ_FORMAT = "<BHI"
CHUNK = LIMITS["chunk_data_bytes"]
STATUS_VERSION = 1
FLAG_PICTURE = 0x01


class LinkError(ValueError):
    """An answer that cannot be believed; the reason is in the message."""


@dataclass(frozen=True)
class Status:
    zone_level: int
    cpu_pct: int
    temperature_c: float
    picture_available: bool
    uptime_s: int


def encode_status(zone_level: int, cpu_pct: int, temperature_c: float, picture: bool, uptime_s: int, sequence: int) -> bytes:
    payload = struct.pack(STATUS_FORMAT, STATUS_VERSION, zone_level, cpu_pct, int(round(temperature_c * 10)), FLAG_PICTURE if picture else 0, uptime_s)
    return boot.encode(T_STATUS, sequence, payload)


def decode_status(frame: bytes, sequence: int) -> Status:
    kind, seq, payload = _decode(frame, sequence)
    if kind != T_STATUS or len(payload) != struct.calcsize(STATUS_FORMAT):
        raise LinkError("not a status")
    version, zone, cpu, temp, flags, uptime = struct.unpack(STATUS_FORMAT, payload)
    if version != STATUS_VERSION:
        raise LinkError("a status of another version is not believed")
    if zone > 5 or cpu > 100:
        raise LinkError("a value out of range")
    return Status(zone, cpu, temp / 10, bool(flags & FLAG_PICTURE), uptime)


def _decode(frame: bytes, sequence: int) -> tuple[int, int, bytes]:
    try:
        kind, seq, payload = boot.decode(frame, HOST_TYPES)
    except boot.FrameError as error:
        raise LinkError(str(error)) from error
    if seq != sequence & 0xFFFF:
        raise LinkError("an answer for another request is never believed")
    return kind, seq, payload


def encode_chunk(picture_id: int, total: int, offset: int, data: bytes, sequence: int) -> bytes:
    if len(data) > CHUNK:
        raise LinkError("a chunk is at most %d bytes" % CHUNK)
    return boot.encode(T_SNAPSHOT, sequence, struct.pack(SNAPSHOT_HEADER, picture_id, total, offset) + data)


@dataclass
class Assembler:
    """Reassembles one picture from chunks that must arrive in order; anything odd drops the picture, never shows half of one."""

    max_bytes: int
    picture_id: int | None = None
    total: int = 0
    data: bytearray = field(default_factory=bytearray)
    dropped: int = 0

    def reset(self) -> None:
        self.picture_id, self.total, self.data = None, 0, bytearray()

    def feed(self, picture_id: int, total: int, offset: int, chunk: bytes) -> bytes | None:
        """The complete JPEG when this chunk finishes it, else None."""
        if total == 0 or total > self.max_bytes:
            self.dropped += 1
            self.reset()
            return None
        if self.picture_id != picture_id:
            if offset != 0:
                self.dropped += 1
                return None
            self.picture_id, self.total, self.data = picture_id, total, bytearray()
        if total != self.total or offset != len(self.data) or offset + len(chunk) > total:
            self.dropped += 1
            self.reset()
            return None
        self.data += chunk
        if len(self.data) < total:
            return None
        picture = bytes(self.data)
        self.reset()
        if not (picture.startswith(b"\xff\xd8") and picture.endswith(b"\xff\xd9")):
            self.dropped += 1
            return None
        return picture


class FakeJetson:
    """The host side as a double. `mode`: ok, silent (never answers), corrupt (every answer has a bad CRC), hang_in_picture (answers until the middle of a picture)."""

    def __init__(self, picture: bytes, zone_level: int = 0) -> None:
        self.picture, self.zone_level, self.mode = picture, zone_level, "ok"
        self.picture_id = 1
        self.uptime_s = 0

    def next_picture(self, picture: bytes) -> None:
        """The camera produced a newer picture."""
        self.picture, self.picture_id = picture, self.picture_id + 1

    def answer(self, request: bytes, now_ms: int) -> bytes | None:
        if self.mode == "silent":
            return None
        try:
            kind, sequence, payload = boot.decode(request, MASTER_TYPES)
        except boot.FrameError:
            return None
        if kind == T_STATUS_REQ:
            frame = encode_status(self.zone_level, 23, 52.0, True, now_ms // 1000, sequence)
        else:
            camera, last, offset = struct.unpack(SNAPSHOT_REQ_FORMAT, payload)
            if camera != 0:
                frame = boot.encode(T_SNAPSHOT_NONE, sequence, bytes([0]))
            elif (last >= self.picture_id and offset == 0) or offset >= len(self.picture):
                frame = boot.encode(T_SNAPSHOT_NONE, sequence, bytes([1]))
            elif self.mode == "hang_in_picture" and offset >= len(self.picture) // 2:
                return None
            else:
                frame = encode_chunk(self.picture_id, len(self.picture), offset, self.picture[offset : offset + CHUNK], sequence)
        if self.mode == "corrupt":
            frame = frame[:-1] + bytes([frame[-1] ^ 0xFF])
        return frame


@dataclass
class LinkMaster:
    """The master side: polls the status every status_period_ms and the picture every snapshot_period_ms, and reports what it heard as events of the view-model."""

    host: FakeJetson
    max_jpeg_bytes: int
    sequence: int = 0
    last_status: int = -10**9
    last_snapshot: int = -10**9
    last_picture_id: int = 0
    assembler: Assembler = field(init=False)
    pictures: list[bytes] = field(default_factory=list)
    status: Status | None = None
    transactions: int = 0
    wire_bytes: int = 0

    def __post_init__(self) -> None:
        self.assembler = Assembler(self.max_jpeg_bytes)

    def _ask(self, kind: int, payload: bytes, now_ms: int) -> bytes | None:
        self.sequence = (self.sequence + 1) & 0xFFFF
        request = boot.encode(kind, self.sequence, payload)
        answer = self.host.answer(request, now_ms)
        self.transactions += 1
        self.wire_bytes += len(request) + (len(answer) if answer else 0)
        return answer

    def step(self, now_ms: int) -> list[tuple[str, tuple[int, ...]]]:
        """Runs what is due at this time. Returns the events for the view-model, in order."""
        events: list[tuple[str, tuple[int, ...]]] = []
        if now_ms - self.last_status >= LIMITS["status_period_ms"]:
            self.last_status = now_ms
            answer = self._ask(T_STATUS_REQ, b"", now_ms)
            try:
                if answer is None:
                    raise LinkError("no answer")
                self.status = decode_status(answer, self.sequence)
                events.append(("HB", (self.status.zone_level,)))
                events.append(("SPI_OK", ()))
            except LinkError:
                events.append(("SPI_ERR", ()))
        if self.status is not None and self.status.picture_available and now_ms - self.last_snapshot >= LIMITS["snapshot_period_ms"]:
            self._snapshot(now_ms)
        return events

    def _snapshot(self, now_ms: int) -> None:
        """Fetches chunks until a picture is complete or something goes wrong; a failure costs only the picture."""
        self.last_snapshot = now_ms
        for _ in range(self.max_jpeg_bytes // CHUNK + 2):
            answer = self._ask(T_SNAPSHOT_REQ, struct.pack(SNAPSHOT_REQ_FORMAT, 0, self.last_picture_id, len(self.assembler.data)), now_ms)
            if answer is None:
                self.assembler.reset()
                return
            try:
                kind, _, payload = _decode(answer, self.sequence)
            except LinkError:
                self.assembler.dropped += 1
                self.assembler.reset()
                return
            if kind == T_SNAPSHOT_NONE:
                return
            if kind != T_SNAPSHOT or len(payload) < struct.calcsize(SNAPSHOT_HEADER):
                self.assembler.reset()
                return
            picture_id, total, offset = struct.unpack_from(SNAPSHOT_HEADER, payload)
            done = self.assembler.feed(picture_id, total, offset, payload[struct.calcsize(SNAPSHOT_HEADER) :])
            if self.assembler.picture_id is None and done is None:
                return  # dropped
            if done is not None:
                self.pictures.append(done)
                self.last_picture_id = picture_id
                return


def make_jpeg(size: int, seed: int = 1) -> bytes:
    """A stand-in for a JPEG picture of a given size: the start and end markers around bytes of a fixed pseudo-random sequence."""
    body = bytearray()
    value = seed
    while len(body) < size - 4:
        value = (value * 1103515245 + 12345) & 0x7FFFFFFF
        body.append((value >> 16) & 0xFF)
    return b"\xff\xd8" + bytes(body) + b"\xff\xd9"
