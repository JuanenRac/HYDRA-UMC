#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/ui_jetson_side.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The Jetson side of the exchange with the display interface, as a protocol-level service in Python (`src/ui_h757/spec/jetson_link.json`).

`JetsonSide.answer(request, now_ms)` takes the bytes of one request of the master and returns the bytes of the answer, or None when the request must not be answered. It holds no
state per request: the status comes from a function (the verdict of the safety-zones service, the load and the temperature of the machine) and the pictures from a source of
cameras (the newest JPEG of each and its number), so a chunk lost on the wire is simply asked again. What a real Jetson needs besides this (the SPI slave endpoint, the zones service
and the camera capture) is not here: this is the part that decides what to say, tested against the master reference of `tools/ui_link.py`. Nothing here ran on a Jetson.
Evidence: simulator."""
from __future__ import annotations

import struct
import sys
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import spi_boot_master as boot  # noqa: E402
import ui_link as link  # noqa: E402

NO_CAMERA, NO_NEWER, BUSY = 0, 1, 2


@dataclass(frozen=True)
class StatusReading:
    zone_level: int  # 0 ready .. 5 unknown
    cpu_pct: int
    temperature_c: float


class CameraSource:
    """The newest picture of each camera: a number that grows with every new picture, and the JPEG."""

    def __init__(self) -> None:
        self.pictures: dict[int, tuple[int, bytes]] = {}
        self.busy: set[int] = set()

    def publish(self, camera: int, picture: bytes) -> int:
        number = self.pictures.get(camera, (0, b""))[0] + 1
        self.pictures[camera] = (number & 0xFFFF, picture)
        return number & 0xFFFF


class JetsonSide:
    def __init__(self, status: Callable[[int], StatusReading], cameras: CameraSource, boot_ms: int = 0) -> None:
        self.status, self.cameras, self.boot_ms = status, cameras, boot_ms
        self.ignored = 0

    def answer(self, request: bytes, now_ms: int) -> bytes | None:
        try:
            kind, sequence, payload = boot.decode(request, link.MASTER_TYPES)
        except boot.FrameError:
            self.ignored += 1  # a damaged or unknown request is not answered: the master sees the silence
            return None
        if kind == link.T_STATUS_REQ:
            if payload:
                self.ignored += 1
                return None
            reading = self.status(now_ms)
            level = reading.zone_level if 0 <= reading.zone_level <= 5 else 5  # whatever the service says, the panel never reads a value out of range
            has_picture = 0 in self.cameras.pictures and 0 not in self.cameras.busy
            return link.encode_status(level, max(0, min(100, reading.cpu_pct)), reading.temperature_c, has_picture, max(0, now_ms - self.boot_ms) // 1000, sequence)
        if len(payload) != struct.calcsize(link.SNAPSHOT_REQ_FORMAT):
            self.ignored += 1
            return None
        camera, last_picture, offset = struct.unpack(link.SNAPSHOT_REQ_FORMAT, payload)
        if camera not in self.cameras.pictures:
            return boot.encode(link.T_SNAPSHOT_NONE, sequence, bytes([NO_CAMERA]))
        if camera in self.cameras.busy:
            return boot.encode(link.T_SNAPSHOT_NONE, sequence, bytes([BUSY]))
        number, picture = self.cameras.pictures[camera]
        if (last_picture == number and offset == 0) or offset >= len(picture):
            return boot.encode(link.T_SNAPSHOT_NONE, sequence, bytes([NO_NEWER]))
        return link.encode_chunk(number, len(picture), offset, picture[offset : offset + link.CHUNK], sequence)
