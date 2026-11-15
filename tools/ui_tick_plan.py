# =============================================================================
# HYDRA-UMC - tools/ui_tick_plan.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Reference of the tick and the pace of the interface library (the C twin is src/mcu_stm32h757/Common/ui_tick_plan.h) and the reader of the [tick] section of config/lvgl.cfg.
Evidence: simulator."""
from __future__ import annotations

import configparser
from pathlib import Path

CFG_PATH = Path(__file__).resolve().parents[1] / "config" / "lvgl.cfg"
BOUND_MAX_MS = 1000
CATCHUP_MAX_MS = 10000
NO_TIMER_READY = 0xFFFFFFFF
WORD = 0xFFFFFFFF


def load_config(path: Path = CFG_PATH) -> dict:
    parser = configparser.ConfigParser(inline_comment_prefixes=None)
    parser.read_string(path.read_text(encoding="utf-8"))
    section = parser["tick"]
    cfg = {"catchup_max_ms": int(section["catchup_max_ms"]), "handler_min_ms": int(section["handler_min_ms"]), "handler_max_ms": int(section["handler_max_ms"])}
    check(cfg)
    return cfg


def check(cfg: dict) -> None:
    if not 1 <= cfg["handler_min_ms"] <= cfg["handler_max_ms"] <= BOUND_MAX_MS or not 1 <= cfg["catchup_max_ms"] <= CATCHUP_MAX_MS:
        raise ValueError("config/lvgl.cfg: handler bounds 1..1000 ms with min <= max, catchup_max_ms 1..10000")


def tick(cfg: dict, last_ms: int, now_ms: int) -> tuple[int, bool]:
    """(milliseconds to feed the library, whether the stall was held to the catch-up); the counter is a 32-bit one and wraps."""
    check(cfg)
    elapsed = (now_ms - last_ms) & WORD
    if elapsed > cfg["catchup_max_ms"]:
        return cfg["catchup_max_ms"], True
    return elapsed, False


def sleep_ms(cfg: dict, asked_ms: int) -> int:
    """How long the interface task sleeps: what the library asked for, held between the bounds."""
    return max(cfg["handler_min_ms"], min(cfg["handler_max_ms"], asked_ms))
