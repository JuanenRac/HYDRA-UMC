#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/spi_system.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Reference and host client of the commands of the SPI link that the CM4 answers itself (see src/mcu_stm32h745/CM4/spi_system.h, docs/SPI_SYSTEM.md).

    0x20 STOP_REQUEST       'S' 'T' 'O' 'P' reason           ask the brain to put the stack in the emergency stop
    0x21 RELEASE_REQUEST    'R' 'E' 'L' 'S'                  a person asked to end the stop
    0x22 TELEMETRY_QUERY    selector (0 = the CM4)
    0x23 TELEMETRY_RESPONSE selector, flags, crc errors, retries, sequence gaps, tx errors, rx errors, state
    0x24 PROBE_REQUEST      scan id                          scan the slots before an update
    0x25 PROBE_RESPONSE     scan id, then the verdicts of slots 0..7 in a nibble each
    0x26 ZONES_HEARTBEAT    'Z' 'O' 'N' 'E'                  the safety-zones chain is healthy; the application stops the stack when it stops coming

None of these frames is a CAN message: the tier of the frame (SELF) decides where it goes before its type is read, although the types 0x20 and up are also offsets of the 64-identifier window of a slot (the F-RAM requests). The firmware answers them only when it is built
with HYDRA_SPI_SYSTEM=1 (telemetry and probe) and HYDRA_SPI_STOP_COMMAND=1 (stop and release). Evidence of anything built on this file: simulator."""
from __future__ import annotations

from dataclasses import dataclass, field

STOP_REQUEST = 0x20
RELEASE_REQUEST = 0x21
TELEMETRY_QUERY = 0x22
TELEMETRY_RESPONSE = 0x23
PROBE_REQUEST = 0x24
PROBE_RESPONSE = 0x25
ZONES_HEARTBEAT = 0x26

STATE_BOOTLOADER = 0x01
STATE_APPLICATION = 0x02
FLAG_LINK_OK = 0x01
FLAG_ESTOP_LATCHED = 0x02
FLAG_STOP_ENABLED = 0x04

VERDICTS = {"ABSENT": 0, "OK": 1, "DUPLICATE": 2, "MISPLACED": 3, "NOISY": 4}
VERDICT_NAMES = {value: name for name, value in VERDICTS.items()}


@dataclass(frozen=True)
class Config:
    telemetry_enabled: bool = False
    stop_enabled: bool = False


@dataclass
class Snapshot:
    crc_errors: int = 0
    retries: int = 0
    seq_gaps: int = 0
    can_tx_errors: int = 0
    can_rx_errors: int = 0
    state: int = STATE_APPLICATION
    link_ok: bool = True
    estop_latched: bool = True
    scan_verdicts: list[int] = field(default_factory=lambda: [0] * 8)


def saturate(value: int) -> int:
    return min(max(value, 0), 255)


def stop_payload(reason: int) -> bytes:
    return b"STOP" + bytes((reason & 0xFF,))


def release_payload() -> bytes:
    return b"RELS"


def zones_heartbeat_payload() -> bytes:
    return b"ZONE"


def telemetry(config: Config, snap: Snapshot, selector: int = 0) -> bytes:
    flags = (FLAG_LINK_OK if snap.link_ok else 0) | (FLAG_ESTOP_LATCHED if snap.estop_latched else 0) | (FLAG_STOP_ENABLED if config.stop_enabled else 0)
    return bytes((selector, flags, saturate(snap.crc_errors), saturate(snap.retries), saturate(snap.seq_gaps), snap.can_tx_errors, snap.can_rx_errors, snap.state))


def probe_response(snap: Snapshot, scan_id: int) -> bytes:
    nibbles = [v & 0x0F for v in snap.scan_verdicts]
    packed = [nibbles[2 * i] | (nibbles[2 * i + 1] << 4) for i in range(4)]
    return bytes([scan_id & 0xFF, *packed])


def handle(config: Config, snap: Snapshot, frame_type: int, payload: bytes) -> tuple[str, bytes]:
    """The action the firmware takes for one frame of the tier SELF: ('none'|'reply'|'trip'|'release'|'scan'|'heartbeat', bytes). A reply carries the answer
    (the type is TELEMETRY_RESPONSE); trip carries the reason; scan carries the scan id."""
    if not config.telemetry_enabled:
        return "none", b""
    if frame_type == TELEMETRY_QUERY:
        return ("reply", telemetry(config, snap, 0)) if payload == b"\x00" else ("none", b"")
    if frame_type == PROBE_REQUEST:
        return ("scan", payload) if len(payload) == 1 else ("none", b"")
    if frame_type == STOP_REQUEST:
        if config.stop_enabled and len(payload) == 5 and payload[:4] == b"STOP":
            return "trip", payload[4:5]
        return "none", b""
    if frame_type == RELEASE_REQUEST:
        return ("release", b"") if config.stop_enabled and payload == b"RELS" else ("none", b"")
    if frame_type == ZONES_HEARTBEAT:
        return ("heartbeat", b"") if config.stop_enabled and payload == b"ZONE" else ("none", b"")
    return "none", b""


@dataclass(frozen=True)
class Telemetry:
    selector: int
    link_ok: bool
    estop_latched: bool
    stop_enabled: bool
    crc_errors: int
    retries: int
    seq_gaps: int
    can_tx_errors: int
    can_rx_errors: int
    state: int


def decode_telemetry(data: bytes) -> Telemetry | None:
    if len(data) != 8:
        return None
    return Telemetry(data[0], bool(data[1] & FLAG_LINK_OK), bool(data[1] & FLAG_ESTOP_LATCHED), bool(data[1] & FLAG_STOP_ENABLED), data[2], data[3], data[4], data[5], data[6], data[7])


def decode_probe_response(data: bytes) -> tuple[int, list[str]] | None:
    if len(data) != 5:
        return None
    verdicts = []
    for byte in data[1:5]:
        verdicts += [VERDICT_NAMES.get(byte & 0x0F, "UNKNOWN"), VERDICT_NAMES.get(byte >> 4, "UNKNOWN")]
    return data[0], verdicts


class Client:
    """The host side (the CM5 bridge): builds the frames and reads the answers through any function `exchange(frame_type, payload) -> (type, payload)`,
    a real SPI transport or the fake one of the tests."""

    def __init__(self, exchange):
        self._exchange = exchange

    def read_telemetry(self) -> Telemetry | None:
        answer_type, answer = self._exchange(TELEMETRY_QUERY, b"\x00")
        return decode_telemetry(answer) if answer_type == TELEMETRY_RESPONSE else None

    def scan(self, scan_id: int) -> tuple[int, list[str]] | None:
        answer_type, answer = self._exchange(PROBE_REQUEST, bytes((scan_id & 0xFF,)))
        return decode_probe_response(answer) if answer_type == PROBE_RESPONSE else None

    def stop(self, reason: int) -> None:
        self._exchange(STOP_REQUEST, stop_payload(reason))

    def release(self) -> None:
        self._exchange(RELEASE_REQUEST, release_payload())

    def zones_heartbeat(self) -> None:
        self._exchange(ZONES_HEARTBEAT, zones_heartbeat_payload())
