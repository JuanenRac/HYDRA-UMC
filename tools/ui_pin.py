#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/ui_pin.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Reference of the store of the operator PIN and of the pad that types it (the port of the view-model, `docs/DSI_EN_H757.md`).

The view-model never sees the digits: the pad collects them, the store checks them and the pad tells the view-model "right" or "wrong" (`PIN_OK`, `PIN_BAD`). The store keeps
one record of 72 bytes in two slots of the external F-RAM (the same chip as the other settings): a salted PBKDF2-HMAC-SHA256 hash of the digits, never the digits; a failure
counter that is written BEFORE the comparison (a power cut cannot be used to try again for free) and a CRC; the slot with the higher sequence number that is valid wins, so a
write torn by a power cut leaves the old record, never a half one. A record that cannot be read is "unavailable": the PIN is never accepted from it and the panel stays locked until
a person with the Studio resets it; STOP does not depend on any of this. The number of iterations and the lengths are in `config/ui.cfg` ([pin]).
Evidence: simulator; nothing here has run on a board, and the cost of the iterations on the Cortex-M7 is not measured."""
from __future__ import annotations

import configparser
import hashlib
import hmac
import struct
import zlib
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAGIC = b"PIN1"
RECORD = 72
SLOTS = 2
FORMAT = "<4sBBHI16s32sBB"  # magic, version, digits, sequence, iterations, salt, hash, failures, spare
BODY = struct.calcsize(FORMAT)  # 61
assert BODY + 4 <= RECORD
VERSION = 1
OK, BAD, UNAVAILABLE = "ok", "bad", "unavailable"


def load_settings(path: Path = ROOT / "config" / "ui.cfg") -> dict[str, int]:
    parser = configparser.ConfigParser(inline_comment_prefixes=None)
    parser.read_string(path.read_text(encoding="utf-8"))
    section = parser["pin"]
    values = {name: int(section[name]) for name in ("pin_hash_iterations", "pin_min_digits", "pin_max_digits", "pin_cycles_per_block", "pin_hw_cycles_per_block", "pin_check_max_ms")}
    if not (1 <= values["pin_min_digits"] <= values["pin_max_digits"] <= 12 and 1000 <= values["pin_hash_iterations"] <= 1_000_000):
        raise ValueError("the [pin] values of config/ui.cfg are out of range")
    return values


class TornWrite(Exception):
    """Raised by the F-RAM double when the power is cut in the middle of a write."""


class Fram:
    """The F-RAM as a double: bytes, and an optional cut of the power after a number of written bytes (to test a torn write)."""

    def __init__(self) -> None:
        self.data = bytearray(b"\xff" * (RECORD * SLOTS))
        self.cut_after: int | None = None

    def write(self, offset: int, chunk: bytes) -> None:
        for index, byte in enumerate(chunk):
            if self.cut_after is not None:
                if self.cut_after == 0:
                    self.cut_after = None
                    raise TornWrite()
                self.cut_after -= 1
            self.data[offset + index] = byte


@dataclass(frozen=True)
class Record:
    digits: int
    sequence: int
    iterations: int
    salt: bytes
    hash: bytes
    failures: int


def _hash(pin: str, salt: bytes, iterations: int) -> bytes:
    return hashlib.pbkdf2_hmac("sha256", pin.encode("ascii"), salt, iterations, dklen=32)


def _pack(record: Record) -> bytes:
    body = struct.pack(FORMAT, MAGIC, VERSION, record.digits, record.sequence & 0xFFFF, record.iterations, record.salt, record.hash, record.failures, 0)
    return (body + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)).ljust(RECORD, b"\xff")


def _unpack(raw: bytes) -> Record | None:
    body, crc = raw[:BODY], struct.unpack_from("<I", raw, BODY)[0]
    if crc != zlib.crc32(body) & 0xFFFFFFFF:
        return None
    magic, version, digits, sequence, iterations, salt, digest, failures, _ = struct.unpack(FORMAT, body)
    if magic != MAGIC or version != VERSION:
        return None
    return Record(digits, sequence, iterations, salt, digest, failures)


class PinStore:
    def __init__(self, fram: Fram, settings: dict[str, int] | None = None) -> None:
        self.fram = fram
        self.settings = settings or load_settings()

    # ------------------------------------------------------------ the slots
    def _records(self) -> list[tuple[int, Record]]:
        found = []
        for slot in range(SLOTS):
            record = _unpack(bytes(self.fram.data[slot * RECORD : (slot + 1) * RECORD]))
            if record is not None:
                found.append((slot, record))
        return found

    def _current(self) -> tuple[int, Record] | None:
        """The valid record with the higher sequence (sequence numbers wrap: the newer is the one ahead by less than half the range)."""
        found = self._records()
        if not found:
            return None
        best = found[0]
        for candidate in found[1:]:
            if (candidate[1].sequence - best[1].sequence) & 0xFFFF < 0x8000 and candidate[1].sequence != best[1].sequence:
                best = candidate
        return best

    def _write(self, record: Record) -> None:
        current = self._current()
        slot = 0 if current is None else 1 - current[0]
        self.fram.write(slot * RECORD, _pack(record))

    # ------------------------------------------------------------ what the panel asks
    def state(self) -> str:
        """unset: never set, or erased; set: a valid record; corrupt: something is there that cannot be read."""
        if self._current() is not None:
            return "set"
        erased = all(byte == 0xFF for byte in self.fram.data)
        return "unset" if erased else "corrupt"

    def failures(self) -> int:
        current = self._current()
        return current[1].failures if current else 0

    def set_pin(self, pin: str, salt: bytes) -> None:
        """Sets the PIN (done from the Studio, not from the panel). Refuses a PIN of the wrong length, not made of digits or with one digit only."""
        low, high = self.settings["pin_min_digits"], self.settings["pin_max_digits"]
        if not pin.isascii() or not pin.isdigit() or not low <= len(pin) <= high:
            raise ValueError(f"a PIN is {low} to {high} digits")
        if len(set(pin)) == 1:
            raise ValueError("a PIN of one repeated digit is refused")
        if len(salt) != 16:
            raise ValueError("the salt is 16 bytes")
        current = self._current()
        sequence = 0 if current is None else (current[1].sequence + 1) & 0xFFFF
        iterations = self.settings["pin_hash_iterations"]
        self._write(Record(len(pin), sequence, iterations, salt, _hash(pin, salt, iterations), 0))

    def erase(self) -> None:
        """The reset from the Studio: leaves the F-RAM as new, with no PIN."""
        self.fram.data = bytearray(b"\xff" * (RECORD * SLOTS))

    def verify(self, pin: str) -> str:
        """OK, BAD or UNAVAILABLE. The failure counter goes to the F-RAM before the comparison and back to zero after a right PIN."""
        current = self._current()
        if current is None:
            return UNAVAILABLE
        record = current[1]
        if not pin.isascii() or not pin.isdigit():
            return BAD
        try:
            self._write(Record(record.digits, (record.sequence + 1) & 0xFFFF, record.iterations, record.salt, record.hash, min(255, record.failures + 1)))
        except TornWrite:
            return UNAVAILABLE  # the old record is still there; the attempt does not count as a success
        candidate = _hash(pin, record.salt, record.iterations)
        if hmac.compare_digest(candidate, record.hash):
            after = self._current()
            assert after is not None
            try:
                self._write(Record(record.digits, (after[1].sequence + 1) & 0xFFFF, record.iterations, record.salt, record.hash, 0))
            except TornWrite:
                pass  # the PIN was right; the counter stays one up, which only costs a try
            return OK
        return BAD


class PinPad:
    """The pad of the panel: collects digits from the actions of the screen and reports to the view-model.

    `touch(action)` takes the id of an action of `ui_spec.json` (`pin_digit_N`, `pin_clear`, `pin_enter`) and returns the event for the view-model, or None."""

    def __init__(self, store: PinStore) -> None:
        self.store = store
        self.digits = ""

    def boot_event(self) -> tuple[str, tuple[int, ...]]:
        """What the port tells the view-model at start: the wrong PINs the store has counted since the last right one, so a power cut is not a way to get new tries."""
        return ("PIN_FAILURES", (self.store.failures(),))

    def touch(self, action: str) -> tuple[str, tuple[int, ...]] | None:
        high = self.store.settings["pin_max_digits"]
        low = self.store.settings["pin_min_digits"]
        if action.startswith("pin_digit_"):
            if len(self.digits) < high:
                self.digits += action[-1]
            return None
        if action == "pin_clear":
            self.digits = ""
            return None
        if action == "pin_enter":
            typed, self.digits = self.digits, ""
            if len(typed) < low:
                return None  # too short to be a try: nothing is counted
            return ("PIN_OK", ()) if self.store.verify(typed) == OK else ("PIN_BAD", ())
        return None
