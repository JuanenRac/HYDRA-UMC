#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/spi_frame.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Reference of the sealed 128-byte frame of the SPI link (see spi_frame_crc.h): marker, sequence and CRC-16/CCITT-FALSE after the 12 bytes."""
from __future__ import annotations

FRAME_SIZE = 128
MARKER = 0xA1
OFS_MARKER = 12
OFS_SEQ = 13
OFS_CRC = 14
CRC_SPAN = 14


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def build(tier: int, slot: int, frame_type: int, payload: bytes = b"", sequence: int | None = None) -> bytes:
    """A frame as the firmware lays it out: tier, slot, type, length, 8 payload bytes, padding 0xFF; sealed when a sequence is given."""
    if len(payload) > 8:
        raise ValueError("the payload of a frame is at most 8 bytes")
    wire = bytearray(b"\xff" * FRAME_SIZE)
    wire[0], wire[1], wire[2], wire[3] = tier, slot, frame_type, len(payload)
    wire[4:12] = payload.ljust(8, b"\x00")
    if sequence is not None:
        wire[OFS_MARKER] = MARKER
        wire[OFS_SEQ] = sequence & 0xFF
        crc = crc16(bytes(wire[:CRC_SPAN]))
        wire[OFS_CRC], wire[OFS_CRC + 1] = crc >> 8, crc & 0xFF
    return bytes(wire)


def check(wire: bytes) -> str:
    """'unsealed' (older padding), 'ok' or 'bad_crc'."""
    if len(wire) != FRAME_SIZE:
        raise ValueError("a frame is 128 bytes")
    if wire[OFS_MARKER] != MARKER:
        return "unsealed"
    return "ok" if crc16(wire[:CRC_SPAN]) == (wire[OFS_CRC] << 8 | wire[OFS_CRC + 1]) else "bad_crc"


def judge(wire: bytes, last: int | None) -> str:
    """What the receiver does: 'dispatch', 'duplicate', 'bad_crc' or 'unsealed'."""
    state = check(wire)
    if state != "ok":
        return state
    return "duplicate" if last is not None and wire[OFS_SEQ] == last else "dispatch"
