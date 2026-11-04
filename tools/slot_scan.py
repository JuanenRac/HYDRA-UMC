#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/slot_scan.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Host reference of the slot scan: find which of the eight slots hold a board and which hold two boards with the same identifier.

docs/SLOT_SCAN.md is the contract; docs/bus_contract.json (section slot_scan) holds its numbers; tests/slot_scan_vectors.json holds
the frames and the scenarios that both this file and, one day, the firmware must reproduce. Nothing here has run on a board.

The board identifier is a 3-bit DIP switch, so nothing stops two boards from being set to the same slot. Both would answer the same
identifiers with different data, which on CAN ends in error frames and no readable answer. The remedy is that a board delays its
answer by a number of steps taken from its own unique identifier, so two boards of one slot answer one after the other and the host
sees two different unique identifiers in one window. A scan that sees that (or a window with bus errors and no readable answer) never
lets an update write to the slot.
"""
from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path

CONTRACT_PATH = Path(__file__).resolve().parents[1] / "docs" / "bus_contract.json"

ROLE_BOOTLOADER = 0
ROLE_APPLICATION = 1

ABSENT = "ABSENT"
OK = "OK"
DUPLICATE = "DUPLICATE"
MISPLACED = "MISPLACED"
NOISY = "NOISY"

SLOTS = 8


def load_contract() -> dict:
    return json.loads(CONTRACT_PATH.read_text(encoding="utf-8"))["slot_scan"]


def _int(value: str) -> int:
    return int(value, 16)


def encode_probe(scan_id: int, magic: int | None = None) -> bytes:
    """Probe: magic, scan id, four zero bytes, then the complements of magic and scan id."""
    magic = _int(load_contract()["probe_magic"]) if magic is None else magic
    if not (0 <= scan_id < 256):
        raise ValueError("scan id out of range")
    return bytes([magic, scan_id, 0, 0, 0, 0, magic ^ 0xFF, scan_id ^ 0xFF])


def decode_probe(data: bytes) -> int | None:
    """The scan id, or None when the frame is not a well-formed probe."""
    if len(data) != 8 or data[0] != _int(load_contract()["probe_magic"]):
        return None
    if data[2:6] != bytes(4) or data[6] != data[0] ^ 0xFF or data[7] != data[1] ^ 0xFF:
        return None
    return data[1]


def encode_reply(board_id: int, role: int, scan_id: int, uid32: int) -> bytes:
    """Reply: magic, board id (low nibble) and role (high nibble), scan id, unique identifier (32 bits, little endian), XOR of the seven."""
    if not (0 <= board_id < SLOTS and role in (ROLE_BOOTLOADER, ROLE_APPLICATION) and 0 <= scan_id < 256 and 0 <= uid32 < 1 << 32):
        raise ValueError("field out of range")
    body = bytes([_int(load_contract()["reply_magic"]), board_id | (role << 4), scan_id]) + uid32.to_bytes(4, "little")
    check = 0
    for byte in body:
        check ^= byte
    return body + bytes([check])


@dataclass(frozen=True)
class Reply:
    board_id: int
    role: int
    scan_id: int
    uid32: int


def decode_reply(data: bytes) -> Reply | None:
    if len(data) != 8 or data[0] != _int(load_contract()["reply_magic"]):
        return None
    check = 0
    for byte in data[:7]:
        check ^= byte
    if check != data[7]:
        return None
    board_id, role = data[1] & 0x0F, data[1] >> 4
    if board_id >= SLOTS or role not in (ROLE_BOOTLOADER, ROLE_APPLICATION):
        return None
    return Reply(board_id, role, data[2], int.from_bytes(data[3:7], "little"))


def reply_delay_ms(uid32: int, timing: dict | None = None) -> int:
    """How long a board waits before it answers: a step per value of its unique identifier modulo the number of steps."""
    timing = load_contract()["timing_ms"] if timing is None else timing
    # the multiplier spreads the low bits of the identifier, which a series of boards often shares
    return ((uid32 * 2654435761) & 0xFFFFFFFF) % timing["reply_steps"] * timing["reply_step"]


@dataclass(frozen=True)
class Heard:
    """What the host heard in the window of one slot: the frames that decoded and the number of error frames."""

    frames: tuple[bytes, ...] = ()
    bus_errors: int = 0


def classify(slot: int, heard: Heard, scan_id: int) -> str:
    replies = [r for r in (decode_reply(f) for f in heard.frames) if r is not None and r.scan_id == scan_id]
    uids = {r.uid32 for r in replies}
    if len(uids) > 1:
        return DUPLICATE
    if not replies:
        return NOISY if heard.bus_errors else ABSENT
    if replies[0].board_id != slot:
        return MISPLACED
    # error frames next to a single readable answer: another board of the slot collided with it
    return DUPLICATE if heard.bus_errors else OK


def scan(heard_by_slot: dict[int, Heard], scan_id: int) -> dict[int, str]:
    return {slot: classify(slot, heard_by_slot.get(slot, Heard()), scan_id) for slot in range(SLOTS)}


def write_allowed(result: str) -> bool:
    """An update may write to a slot only when exactly one board is there and the slot is the one it says."""
    return result == OK
