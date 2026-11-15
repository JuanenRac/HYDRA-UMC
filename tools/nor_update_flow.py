# =============================================================================
# HYDRA-UMC - tools/nor_update_flow.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Reference of the backup of the application slot before an update (the C twin is src/mcu_stm32h757/Common/nor_update_flow.h) and the reader of the [previous] and [update] sections of
config/nor_backup.cfg: the two regions of the NOR, the rule that they may not overlap, and the choice made at start-up. `Board` is a simulated NOR and application slot with a power cut
at any operation, to run the whole flow (backup, staging, commit, start-up choice) as the board would. Evidence: simulator."""
from __future__ import annotations

import configparser
import zlib
from pathlib import Path

import nor_backup_plan as nb

CFG_PATH = nb.CFG_PATH
NONE, RESTORE_CURRENT, ROLLBACK_PREVIOUS, LOST = 0, 1, 2, 3


def load_config(path: Path = CFG_PATH) -> dict:
    parser = configparser.ConfigParser(inline_comment_prefixes=None)
    parser.read_string(path.read_text(encoding="utf-8"))
    current = nb.load_config(path)
    previous = dict(current, nor_offset=int(parser["previous"]["offset_bytes"]))
    cfg = {"current": current, "previous": previous, "require_backup": int(parser["update"]["require_backup"])}
    if cfg["require_backup"] not in (0, 1):
        raise ValueError("config/nor_backup.cfg: require_backup is 0 or 1")
    plan(cfg)
    return cfg


def plan(cfg: dict) -> tuple[dict, dict]:
    """The layouts of both regions; ValueError when one does not fit, the slots differ or the regions overlap."""
    current, previous = nb.plan(cfg["current"]), nb.plan(cfg["previous"])
    if cfg["current"]["slot_bytes"] != cfg["previous"]["slot_bytes"]:
        raise ValueError("the two regions must hold the same slot")
    if previous["header_a"] < current["end"] and current["header_a"] < previous["end"]:
        raise ValueError("the regions overlap")
    return current, previous


def version_pack(major: int, minor: int, patch: int) -> int:
    """The version of an application as the one word kept in the header of its copy: 10 bits each; 0 (not known) when a number does not fit."""
    if not all(0 <= part <= 1023 for part in (major, minor, patch)):
        return 0
    return (major << 20) | (minor << 10) | patch


def version_unpack(word: int) -> tuple[int, int, int]:
    """The three numbers of a packed version; a word with bits above the 30 that are used reads as 0.0.0 (as does a copy that kept none)."""
    if word >> 30:
        word = 0
    return (word >> 20) & 1023, (word >> 10) & 1023, word & 1023


def decide(app_good: bool, current_copy: bool, previous_copy: bool) -> int:
    if app_good:
        return NONE
    if current_copy:
        return RESTORE_CURRENT
    return ROLLBACK_PREVIOUS if previous_copy else LOST


def update_allowed(app_good: bool, backup_ok: bool, require_backup: int) -> bool:
    """Whether an update may start: nothing to protect, or the backup worked, or the file lets it go on without."""
    return (not app_good) or backup_ok or not require_backup


# ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
# The simulated board: a NOR and an application slot as bytearrays, the operations of Common/nor_update_flow.h over them, and a power cut at any operation. The C twin runs the same
# sequence in tests/test_nor_update_flow.c; here the cut is an exception, so the state it leaves is what the next start-up finds.
# ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
OK, SKIPPED, E_LAYOUT, E_BACKUP, E_NONE = 0, 1, 3, 4, 5


class PowerCut(Exception):
    """The power went away before the operation could start."""


class NorFailure(Exception):
    """The NOR refused an erase or a write (a chip that does not answer), as opposed to the power going away."""


class CopyError(Exception):
    """A copy could not be restored (no header, a damaged image, or the flash refused it)."""


class Board:
    def __init__(self, cfg: dict, nor_size: int | None = None):
        self.cfg = cfg
        self.regions = plan(cfg)  # raises ValueError for a bad layout
        size = nor_size if nor_size is not None else cfg["current"]["nor_size"]
        self.nor = bytearray(b"\xff" * size)
        self.flash = bytearray(b"\xff" * cfg["current"]["slot_bytes"])
        self.nor_dead = False  # the NOR refuses every erase and write
        self.left: int | None = None  # operations that still work before the power goes (None: never)
        self.done = 0

    def tick(self) -> None:
        self.done += 1
        if self.left is not None:
            if self.left == 0:
                raise PowerCut
            self.left -= 1

    # -- the operations (every write or erase is one tick) --
    def nor_erase(self, offset: int, length: int) -> None:
        self.tick()
        if self.nor_dead:
            raise NorFailure
        self.nor[offset : offset + length] = b"\xff" * length

    def nor_write(self, offset: int, data: bytes) -> None:
        self.tick()
        if self.nor_dead:
            raise NorFailure
        for i, byte in enumerate(data):  # a NOR can only clear bits
            self.nor[offset + i] &= byte

    def flash_erase(self) -> None:
        self.tick()
        self.flash[:] = b"\xff" * len(self.flash)

    def flash_write(self, offset: int, data: bytes) -> None:
        self.tick()
        self.flash[offset : offset + len(data)] = data

    # -- one region --
    def _layout(self, region: str) -> dict:
        return nb.plan(self.cfg[region])

    def headers(self, region: str) -> tuple[dict, int] | None:
        layout, slot = self._layout(region), self.cfg[region]["slot_bytes"]
        raw = [nb.header_decode(bytes(self.nor[at : at + nb.HEADER_BYTES]), slot) for at in (layout["header_a"], layout["header_b"])]
        return nb.newest(raw[0], raw[1])

    def _write_header(self, region: str, length: int, crc32: int, version: int = 0) -> None:
        layout = self._layout(region)
        found = self.headers(region)
        sequence = found[0]["sequence"] + 1 if found else 1
        at = layout["header_b"] if found and found[1] == 0 else layout["header_a"]
        self.nor_erase(at, nb.SECTOR)
        self.nor_write(at, nb.header_encode(sequence & 0xFFFFFFFF, length, crc32, version))

    def save(self, region: str, length: int, version: int = 0) -> None:
        """The application of the flash into the region: image first, header last; `version` is the packed version kept in the header (0: not known)."""
        layout = self._layout(region)
        if not 0 < length <= self.cfg[region]["slot_bytes"]:
            raise ValueError("too big")
        self.nor_erase(layout["image_offset"], -(-length // nb.SECTOR) * nb.SECTOR)
        for done in range(0, length, 256):
            self.nor_write(layout["image_offset"] + done, bytes(self.flash[done : min(done + 256, length)]))
        self._write_header(region, length, zlib.crc32(bytes(self.flash[:length])), version)

    def stage_begin(self, total: int) -> None:
        self.nor_erase(self._layout("current")["image_offset"], -(-total // nb.SECTOR) * nb.SECTOR)

    def stage_page(self, offset: int, data: bytes) -> None:
        self.nor_write(self._layout("current")["image_offset"] + offset, data)

    def stage_finish(self, total: int, crc32: int) -> None:
        start = self._layout("current")["image_offset"]
        if zlib.crc32(bytes(self.nor[start : start + total])) != crc32:
            raise ValueError("copy bad")
        self._write_header("current", total, crc32)

    def copy_exists(self, region: str) -> bool:
        found = self.headers(region)
        if not found:
            return False
        start = self._layout(region)["image_offset"]
        return zlib.crc32(bytes(self.nor[start : start + found[0]["length"]])) == found[0]["crc32"]

    def restore(self, region: str) -> tuple[int, int]:
        """The copy back into the application slot (checked in the NOR first, read back after); returns its length and CRC-32."""
        found = self.headers(region)
        if not found or not self.copy_exists(region):
            raise CopyError("no copy")
        length, crc32 = found[0]["length"], found[0]["crc32"]
        start = self._layout(region)["image_offset"]
        self.flash_erase()
        for done in range(0, length, 256):
            self.flash_write(done, bytes(self.nor[start + done : start + min(done + 256, length)]))
        if zlib.crc32(bytes(self.flash[:length])) != crc32:
            raise CopyError("verify")
        return length, crc32

    # -- the flow --
    def backup_before_update(self, app_good: bool, app_length: int, app_version: int = 0) -> tuple[int, bool]:
        """Before an update: (result, update allowed). A broken application is not saved; a failed save refuses the update when the file requires a backup."""
        if not app_good:
            return SKIPPED, True
        try:
            self.save("previous", app_length, app_version)
        except (NorFailure, ValueError):
            return E_BACKUP, not self.cfg["require_backup"]
        return OK, True

    def recover_for(self, app_good: bool, length: int, crc32: int) -> tuple[int, int, int, int]:
        """The start-up choice against the metadata: (result, action used, restored length, restored CRC-32)."""
        return self.recover_for_versioned(app_good, length, crc32)[:4]

    def recover_for_versioned(self, app_good: bool, length: int, crc32: int) -> tuple[int, int, int, int, int]:
        """The same, with the packed version of the previous application as the fifth item (0 for the current copy and for a copy that kept none)."""
        found = self.headers("current")
        matches = bool(found) and found[0]["length"] == length and found[0]["crc32"] == crc32 and self.copy_exists("current")
        act = decide(app_good, matches, self.copy_exists("previous"))
        if act == NONE:
            return OK, NONE, 0, 0, 0
        if act == RESTORE_CURRENT:
            try:
                restored = self.restore("current")
                return OK, RESTORE_CURRENT, restored[0], restored[1], 0
            except CopyError:
                act = ROLLBACK_PREVIOUS
        if act == ROLLBACK_PREVIOUS:
            try:
                restored = self.restore("previous")
                return OK, ROLLBACK_PREVIOUS, restored[0], restored[1], (self.headers("previous") or ({"version": 0},))[0]["version"]
            except CopyError:
                pass
        return E_NONE, LOST, 0, 0, 0
