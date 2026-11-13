#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/enabling_device.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Reference of the logic of a three-position enabling device (the pendant switch of an operator who stays beside the machine). The C twin is src/mcu_stm32h757/CM4/enabling_device.h.

The device has two redundant channels, each reading the position of the switch: 1 released, 2 held in the middle, 3 pressed hard (panic). Motion is allowed ONLY while both channels read 2
and the device got there from 1. Everything else asks for a stop: position 1, position 3, a fault (the channels disagree for longer than the discrepancy time, or a code that is not 1, 2 or 3)
and, above all, the way back: releasing from 3 passes through 2, and that pass must NOT enable motion again; the device has to be released to 1 first. A position has to be stable for the
debounce time to count, but a stop is never delayed by it: leaving 2 asks for the stop at once. The times are in config/enabling.cfg, not in the code.
The stop asked for is a TRIP (the caller sends it): this file only decides. Evidence: simulator (host tests of the reference and of the C twin against the same vectors); nothing here ran
on a pendant or a board."""
from __future__ import annotations

import configparser
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CFG_PATH = ROOT / "config" / "enabling.cfg"

RELEASED, MIDDLE, PANIC = 1, 2, 3
ST_STOPPED, ST_ARMED, ST_ENABLED, ST_LOCKED_OUT, ST_FAULT = 0, 1, 2, 3, 4
STATE_NAMES = ("stopped", "armed", "enabled", "locked_out", "fault")


def load_config(path: Path = CFG_PATH) -> dict[str, int]:
    parser = configparser.ConfigParser(inline_comment_prefixes=None)
    parser.read_string(path.read_text(encoding="utf-8"))
    section = parser["enabling"]
    values = {name: int(section[name]) for name in ("debounce_ms", "discrepancy_ms")}
    if not (1 <= values["debounce_ms"] <= 200 and values["debounce_ms"] <= values["discrepancy_ms"] <= 2000):
        raise ValueError("config/enabling.cfg: debounce_ms 1..200 and discrepancy_ms from debounce_ms to 2000")
    return values


@dataclass
class EnablingDevice:
    debounce_ms: int = 20
    discrepancy_ms: int = 100
    state: int = ST_STOPPED
    released_since: int | None = None  # both channels have read 1 since this time
    middle_since: int | None = None    # both channels have read 2 since this time
    disagree_since: int | None = None
    stops: int = 0

    def enabled(self) -> bool:
        return self.state == ST_ENABLED

    def _elapsed(self, now_ms: int, since: int | None) -> int:
        return 0 if since is None else (now_ms - since) & 0xFFFFFFFF

    def update(self, now_ms: int, channel_a: int, channel_b: int) -> bool:
        """Feeds the two channels. Returns True when motion is allowed after this update. A stop is asked for (see `stops`) the moment the state leaves ENABLED."""
        was_enabled = self.state == ST_ENABLED
        positions = (RELEASED, MIDDLE, PANIC)
        if channel_a not in positions or channel_b not in positions:
            self.state = ST_FAULT  # a code that is not a position is never trusted; only a clean release leaves it
            self.released_since = self.middle_since = self.disagree_since = None
        elif channel_a != channel_b:
            if self.disagree_since is None:
                self.disagree_since = now_ms
            self.released_since = self.middle_since = None
            if self._elapsed(now_ms, self.disagree_since) > self.discrepancy_ms:
                self.state = ST_FAULT
            elif self.state == ST_ENABLED:
                self.state = ST_STOPPED  # leaving the middle asks for the stop at once; the switch has to be released to arm again
        else:
            self.disagree_since = None
            position = channel_a
            if position == PANIC:
                self.state = ST_LOCKED_OUT  # pressed hard: the stop, and the way back through the middle must not enable
                self.released_since = self.middle_since = None
            elif position == RELEASED:
                self.middle_since = None
                if self.state == ST_ENABLED:
                    self.state = ST_STOPPED
                if self.released_since is None:
                    self.released_since = now_ms
                if self.state in (ST_STOPPED, ST_LOCKED_OUT, ST_FAULT) and self._elapsed(now_ms, self.released_since) >= self.debounce_ms:
                    self.state = ST_ARMED  # released and stable: the next middle position may enable
            else:
                self.released_since = None
                if self.middle_since is None:
                    self.middle_since = now_ms
                if self.state == ST_ARMED and self._elapsed(now_ms, self.middle_since) >= self.debounce_ms:
                    self.state = ST_ENABLED
                # from STOPPED, LOCKED_OUT or FAULT the middle position enables nothing: the switch must be released first
        if was_enabled and self.state != ST_ENABLED:
            self.stops += 1
        return self.state == ST_ENABLED
