#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/expansion_slots.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Reference of the power sequence and fault handling of the three expansion slots (the C twin is src/mcu_stm32h757/Common/expansion_slots.h; docs/EXPANSION_SLOTS.md).

Each slot of the board has an enable line (the fused 24 V of that slot), a fault line from the fuse (low is a fault), a present line (low when a card is in) and a reset line
(low holds the card in reset). The lines of each slot are in docs/platform_profiles.json (`expansion.slots`). The rules, all of them fail-safe:
  - at start every slot is off with its card held in reset, and a slot is powered only when its bit is set in `allowed_mask` of the configuration (empty by default) AND a card is present;
  - power comes first: enable goes high with the card in reset; the fault line is ignored for `fault_blank_ms` (the inrush of the card), then it must be quiet until `power_good_ms`
    has passed; only then reset is released, and the card is READY `reset_release_ms` later;
  - a fault (the fault line low for `fault_debounce_ms` in READY, or at all after the blanking time while powering) cuts the slot and holds the card in reset, and the slot stays FAULT until
    `clear_fault` is called AND the fault line is quiet; after `max_faults` faults in a slot it is LOCKED and only `unlock` (a decision of a person) lets it try again;
  - pulling the card cuts the slot and returns it to ABSENT;
  - a slot that is not allowed never leaves OFF, whatever its lines say.
Evidence: simulator. Nothing here has run on a board."""
from __future__ import annotations

import configparser
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CFG_PATH = ROOT / "config" / "expansion_slots.cfg"
MASK = 0xFFFFFFFF
SLOTS = 3

ST_OFF = 0  # not allowed, or nothing to do
ST_ABSENT = 1  # allowed, no card
ST_POWERING = 2
ST_RESETTING = 3
ST_READY = 4
ST_FAULT = 5
ST_LOCKED = 6


def load_config(path: Path = CFG_PATH) -> dict:
    parser = configparser.ConfigParser(inline_comment_prefixes=None)
    parser.read_string(path.read_text(encoding="utf-8"))
    section = parser["slots"]
    cfg = {name: int(section[name], 0) for name in ("allowed_mask", "fault_blank_ms", "power_good_ms", "reset_release_ms", "fault_debounce_ms", "max_faults")}
    if not 0 <= cfg["allowed_mask"] < (1 << SLOTS):
        raise ValueError("config/expansion_slots.cfg: allowed_mask is a mask of the 3 slots (0..7)")
    if not 1 <= cfg["fault_blank_ms"] <= 1000 or not cfg["fault_blank_ms"] < cfg["power_good_ms"] <= 5000:
        raise ValueError("config/expansion_slots.cfg: fault_blank_ms 1..1000 and below power_good_ms (up to 5000)")
    if not 1 <= cfg["reset_release_ms"] <= 1000 or not 0 <= cfg["fault_debounce_ms"] <= 1000 or not 1 <= cfg["max_faults"] <= 20:
        raise ValueError("config/expansion_slots.cfg: reset_release_ms 1..1000, fault_debounce_ms 0..1000, max_faults 1..20")
    return cfg


@dataclass
class Slot:
    state: int = ST_OFF
    enable: bool = False
    reset_released: bool = False
    since: int = 0
    fault_since: int = 0
    fault_seen: bool = False
    faults: int = 0
    clear_requested: bool = False


@dataclass
class ExpansionSlots:
    cfg: dict
    slots: list[Slot] = field(default_factory=lambda: [Slot() for _ in range(SLOTS)])

    def start(self, now: int) -> None:
        for slot in self.slots:
            slot.state = ST_OFF
            slot.enable = False
            slot.reset_released = False
            slot.since = now
            slot.faults = 0
            slot.clear_requested = False
        self.update_idle()

    def update_idle(self) -> None:
        for index, slot in enumerate(self.slots):
            if slot.state == ST_OFF and self.cfg["allowed_mask"] >> index & 1:
                slot.state = ST_ABSENT

    def _cut(self, slot: Slot, state: int, now: int) -> None:
        slot.enable, slot.reset_released = False, False
        slot.state, slot.since = state, now

    def clear_fault(self, index: int) -> None:
        if self.slots[index].state == ST_FAULT:
            self.slots[index].clear_requested = True

    def unlock(self, index: int, now: int) -> None:
        slot = self.slots[index]
        if slot.state == ST_LOCKED:
            slot.faults = 0
            slot.clear_requested = False
            self._cut(slot, ST_ABSENT, now)

    def update(self, now: int, present: int, fault: int) -> None:
        """`present` and `fault` are masks of the three slots, bit set when a card is in / the fuse reports a fault (the lines are active low; the caller inverts them)."""
        cfg = self.cfg
        for index, slot in enumerate(self.slots):
            allowed = bool(cfg["allowed_mask"] >> index & 1)
            has_card, has_fault = bool(present >> index & 1), bool(fault >> index & 1)
            elapsed = (now - slot.since) & MASK
            if not allowed:
                if slot.state != ST_OFF:
                    self._cut(slot, ST_OFF, now)
                continue
            if slot.state == ST_OFF:
                self._cut(slot, ST_ABSENT, now)
            if slot.state in (ST_POWERING, ST_RESETTING, ST_READY) and not has_card:
                self._cut(slot, ST_ABSENT, now)
                continue
            if slot.state == ST_ABSENT:
                if has_card:
                    slot.enable, slot.reset_released = True, False
                    slot.state, slot.since = ST_POWERING, now
                    slot.fault_seen = False
            elif slot.state == ST_POWERING:
                if elapsed >= cfg["fault_blank_ms"] and has_fault:
                    self._trip(slot, now)
                elif elapsed >= cfg["power_good_ms"]:
                    slot.reset_released = True
                    slot.state, slot.since = ST_RESETTING, now
            elif slot.state == ST_RESETTING:
                if has_fault:
                    self._trip(slot, now)
                elif elapsed >= cfg["reset_release_ms"]:
                    slot.state, slot.since = ST_READY, now
                    slot.fault_seen = False
            elif slot.state == ST_READY:
                if has_fault:
                    if not slot.fault_seen:
                        slot.fault_seen, slot.fault_since = True, now
                    if ((now - slot.fault_since) & MASK) >= cfg["fault_debounce_ms"]:
                        self._trip(slot, now)
                else:
                    slot.fault_seen = False
            elif slot.state == ST_FAULT:
                if slot.clear_requested and not has_fault:
                    slot.clear_requested = False
                    self._cut(slot, ST_ABSENT, now)

    def _trip(self, slot: Slot, now: int) -> None:
        slot.faults += 1
        self._cut(slot, ST_LOCKED if slot.faults >= self.cfg["max_faults"] else ST_FAULT, now)
        slot.fault_seen = False
        slot.clear_requested = False
