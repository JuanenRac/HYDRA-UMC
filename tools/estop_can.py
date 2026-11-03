#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/estop_can.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Host reference of the emergency stop frame on identifier 0x001 and of the state machine a node runs on it.

docs/ESTOP_CAN.md is the contract; docs/bus_contract.json holds its numbers; tests/estop_vectors.json holds the frames and the
scenarios that both this file and, one day, the firmware must reproduce. Nothing here has run on a board.

The rule that shapes everything: a stop must never be refused. Any frame on 0x001 that is not a well-formed RELEASE or ALIVE is
treated as a TRIP. Only the way out of a stop is checked.
"""
from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path

ESTOP_CAN_ID = 0x001

KIND_TRIP = 0x01
KIND_RELEASE = 0x02
KIND_ALIVE = 0x03

ALIVE_OK = 0x00
ALIVE_TRIPPED = 0x01

SOURCE_BRAIN = 0x00  # the only source of RELEASE and ALIVE
SOURCE_SLOT_FIRST, SOURCE_SLOT_LAST = 0x01, 0x08
SOURCE_CM5 = 0x10

REASONS = {
    0x01: "operator",
    0x02: "stop-all",
    0x03: "hardware-input",
    0x04: "safety-zone",
    0x05: "host-link-loss",
    0x06: "controller-fault",
    0x07: "bus-fault",
    0x08: "power-loss",
    0xFF: "unspecified",
}

LATCHED = "LATCHED"
READY = "READY"

CONTRACT_PATH = Path(__file__).resolve().parents[1] / "docs" / "bus_contract.json"


def load_timing() -> dict[str, int]:
    return json.loads(CONTRACT_PATH.read_text(encoding="utf-8"))["emergency_stop"]["timing_ms"]


def encode(kind: int, detail: int, source: int, epoch: int, seq: int) -> bytes:
    """The 8 data bytes: kind, detail, source, epoch, sequence (little endian), then the complement of kind and detail."""
    if not (0 <= kind < 256 and 0 <= detail < 256 and 0 <= source < 256 and 0 <= epoch < 256 and 0 <= seq < 65536):
        raise ValueError("field out of range")
    return bytes([kind, detail, source, epoch, seq & 0xFF, seq >> 8, kind ^ 0xFF, detail ^ 0xFF])


@dataclass(frozen=True)
class Frame:
    kind: int
    detail: int
    source: int
    epoch: int
    seq: int


def decode(data: bytes) -> Frame | None:
    """The frame, or None when it is malformed (a receiver then treats it as a TRIP)."""
    if len(data) != 8:
        return None
    kind, detail, source, epoch, seq_lo, seq_hi, inv_kind, inv_detail = data
    if inv_kind != kind ^ 0xFF or inv_detail != detail ^ 0xFF:
        return None
    if kind == KIND_TRIP:
        if detail not in REASONS or not (source in (SOURCE_BRAIN, SOURCE_CM5) or SOURCE_SLOT_FIRST <= source <= SOURCE_SLOT_LAST):
            return None
    elif kind == KIND_RELEASE:
        if detail != 0x00 or source != SOURCE_BRAIN:
            return None
    elif kind == KIND_ALIVE:
        if detail not in (ALIVE_OK, ALIVE_TRIPPED) or source != SOURCE_BRAIN:
            return None
    else:
        return None
    return Frame(kind, detail, source, epoch, seq_lo | (seq_hi << 8))


def seq_newer(candidate: int, last: int | None) -> bool:
    """True when candidate is ahead of last in the modular order of 16 bits (1..32767 steps ahead)."""
    if last is None:
        return True
    return 1 <= (candidate - last) & 0xFFFF <= 0x7FFF


@dataclass
class Node:
    """What every node that moves or heats something does with the frames on 0x001. Time is in milliseconds."""

    timing: dict[str, int] = field(default_factory=load_timing)
    state: str = LATCHED
    epoch: int | None = None
    last_seq: int | None = None
    last_alive_ms: int | None = None
    drivers_enabled: bool = False

    def _latch(self) -> None:
        self.state = LATCHED
        self.drivers_enabled = False

    def on_frame(self, now_ms: int, data: bytes) -> None:
        frame = decode(data)
        if frame is None or frame.kind == KIND_TRIP:
            self._latch()
            if frame is not None:
                self.epoch = frame.epoch
            return
        newer = seq_newer(frame.seq, self.last_seq)
        if frame.kind == KIND_ALIVE:
            if frame.detail == ALIVE_TRIPPED:
                self._latch()
                self.epoch = frame.epoch
                if newer:
                    self.last_seq = frame.seq
                    self.last_alive_ms = now_ms
                return
            if not newer:  # a frozen or replayed sender does not keep the node alive
                return
            self.last_seq = frame.seq
            self.last_alive_ms = now_ms
            return
        # RELEASE: only from a latched node that heard a live master, for the epoch it knows, and with a newer sequence
        if self.state == LATCHED and newer and frame.epoch == self.epoch and self._alive(now_ms):
            self.last_seq = frame.seq
            self.state = READY  # the drivers stay off: a person enables them with the normal command

    def _alive(self, now_ms: int) -> bool:
        return self.last_alive_ms is not None and now_ms - self.last_alive_ms <= self.timing["alive_timeout"]

    def on_bus_off(self) -> None:
        self._latch()

    def tick(self, now_ms: int) -> None:
        if self.state == READY and not self._alive(now_ms):
            self._latch()

    def enable_drivers(self) -> bool:
        """The normal enable command: refused unless the node is READY."""
        if self.state != READY:
            return False
        self.drivers_enabled = True
        return True


def worst_case_frame_bits() -> int:
    """Longest classic data frame with 8 data bytes: 108 bits, up to 24 stuff bits in the 98 bits from SOF to the CRC, 3 of interframe space."""
    return 108 + (98 - 1) // 4 + 3