#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/zones_frame.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Reference of the filter of the brain for the frame of the safety-zones service on identifier 0x002 (the C twin is src/mcu_stm32h757/CM4/zones_frame.h).

The safety-zones service puts its frames on its own identifier 0x002, never on 0x001 (whose layout is the one of docs/ESTOP_CAN.md). The brain has its OWN filter for it: it accepts the
identifier, checks the frame, and asks for a TRIP with the reason `safety-zone` (0x04) when the frame is a stop request, when it is damaged (a damaged frame can only cause a stop, which is
the safe outcome) or when the heartbeats that had started stop coming. The frame has 8 bytes: kind (1 stop, 2 heartbeat), version 1, sequence (16 bits little endian), subject (16 bits),
the number of zones breached and a CRC-8 (polynomial 0x07) of the first seven. A heartbeat refreshes the clock only when its sequence is newer than the last one (a replayed heartbeat
cannot keep a dead service alive); the check never trips before the first heartbeat, so a machine without that service is not affected, and it is armed again by the next heartbeat after
a loss. The switch HYDRA_ZONES_CAN_FILTER ships off. Evidence: simulator (host tests of the reference and of the C twin against the same vectors); nothing here ran on a board."""
from __future__ import annotations

from dataclasses import dataclass

ZONES_CAN_ID = 0x002
KIND_STOP, KIND_HEARTBEAT = 0x01, 0x02
VERSION = 1
MAX_AGE_MS = 1000
REASON_SAFETY_ZONE = 0x04


def crc8(data: bytes) -> int:
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def frame(kind: int, sequence: int, subject: int = 0, breached: int = 0, version: int = VERSION) -> bytes:
    body = bytes((kind, version, sequence & 0xFF, (sequence >> 8) & 0xFF, subject & 0xFF, (subject >> 8) & 0xFF, breached & 0xFF))
    return body + bytes((crc8(body),))


@dataclass
class ZonesFilter:
    enabled: bool = True
    max_age_ms: int = MAX_AGE_MS
    seen: bool = False
    lost: bool = False
    last_sequence: int = 0
    last_heartbeat_ms: int = 0
    trips: int = 0
    ignored: int = 0

    def on_frame(self, now_ms: int, can_id: int, data: bytes) -> bool:
        """True when the brain must trip (reason safety-zone) because of this frame."""
        if not self.enabled or can_id != ZONES_CAN_ID:
            return False
        if len(data) != 8 or data[1] != VERSION or data[7] != crc8(data[:7]):
            self.trips += 1  # a damaged frame can only cause a stop
            return True
        kind = data[0]
        sequence = data[2] | (data[3] << 8)
        if kind == KIND_STOP:
            self.trips += 1
            return True
        if kind == KIND_HEARTBEAT:
            if self.seen and not 0 < (sequence - self.last_sequence) & 0xFFFF < 0x8000:
                self.ignored += 1  # a replayed or old heartbeat does not refresh the clock
                return False
            self.seen, self.lost, self.last_sequence, self.last_heartbeat_ms = True, False, sequence, now_ms
            return False
        self.ignored += 1  # a valid frame of a kind this version does not know
        return False

    def tick(self, now_ms: int) -> bool:
        """True once when the heartbeats that had started stop coming for longer than max_age_ms."""
        if not self.enabled or not self.seen or self.lost:
            return False
        if (now_ms - self.last_heartbeat_ms) & 0xFFFFFFFF > self.max_age_ms:
            self.lost = True
            self.trips += 1
            return True
        return False
