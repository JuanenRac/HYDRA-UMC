#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/health_frame.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Reference of the health frame of a controller board (see src/mcu_stm32g474/health_frame.h): eight bytes on slot_base + 0x11, once a second.

    marker 0x48 | sequence | flags | tx errors | rx errors | stalls | uptime in units of 16 s (saturating) | check (0xA5 xor the seven bytes)

The firmware sends it only when it is built with HYDRA_HEALTH_FRAME=1. Evidence of anything that uses this file: simulator."""
from __future__ import annotations

from dataclasses import dataclass

OFFSET = 0x11
MARKER = 0x48
PERIOD_MS = 1000
FLAG_ESTOP = 0x01
FLAG_BUSOFF = 0x02
FLAG_WATCHDOG_RESET = 0x04
FLAG_INIT_FAILED = 0x08
FLAG_CONFIRMED = 0x10
FLAG_DRIVERS_ENABLED = 0x20
FLAG_INIT_SAFE_STATE = 0x40
FLAG_NAMES = {
    FLAG_ESTOP: "estop_latched",
    FLAG_BUSOFF: "bus_off_seen",
    FLAG_WATCHDOG_RESET: "watchdog_reset_at_start",
    FLAG_INIT_FAILED: "setup_failed_at_start",
    FLAG_CONFIRMED: "application_confirmed",
    FLAG_DRIVERS_ENABLED: "drivers_enabled",
    FLAG_INIT_SAFE_STATE: "setup_failed_safe_state",
}


def check_byte(first_seven: bytes) -> int:
    value = 0xA5
    for byte in first_seven[:7]:
        value ^= byte
    return value


def build(sequence: int, flags: int, tx_errors: int, rx_errors: int, stalls: int, uptime_ms: int) -> bytes:
    units = min(uptime_ms // 16000, 255)
    body = bytes((MARKER, sequence & 0xFF, flags & 0xFF, tx_errors & 0xFF, rx_errors & 0xFF, min(stalls, 255), units))
    return body + bytes((check_byte(body),))


@dataclass(frozen=True)
class Health:
    sequence: int
    flags: int
    tx_errors: int
    rx_errors: int
    stalls: int
    uptime_s: int  # a multiple of 16; 4080 means "4080 or more"

    def names(self) -> list[str]:
        return [name for bit, name in FLAG_NAMES.items() if self.flags & bit]


def decode(data: bytes) -> Health | None:
    """None for anything that is not a health frame: wrong length, marker or check byte."""
    if len(data) != 8 or data[0] != MARKER or data[7] != check_byte(data):
        return None
    return Health(data[1], data[2], data[3], data[4], data[5], data[6] * 16)


class Sender:
    """What the firmware does once per call to Due(): the same period rule, the bus-off flag reported once, wrapping time."""

    def __init__(self) -> None:
        self.sequence = 0
        self.flags = 0
        self.tx_errors = self.rx_errors = self.stalls = 0
        self._last: int | None = None

    def due(self, now_ms: int) -> bytes | None:
        now_ms &= 0xFFFFFFFF
        if self._last is not None and ((now_ms - self._last) & 0xFFFFFFFF) < PERIOD_MS:
            return None
        self._last = now_ms
        frame = build(self.sequence, self.flags, self.tx_errors, self.rx_errors, self.stalls, now_ms)
        self.sequence = (self.sequence + 1) & 0xFF
        self.flags &= ~FLAG_BUSOFF
        return frame
