# =============================================================================
# HYDRA-UMC - tools/gateway_cfg.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Reader of config/gateway.cfg, the settings of the stack tier of the gateway of the H757 board (src/mcu_stm32h757/Common/gateway_stack_a.h refuses the same limits)."""
from __future__ import annotations

import configparser
from pathlib import Path

CFG_PATH = Path(__file__).resolve().parents[1] / "config" / "gateway.cfg"
TIMEOUT_MIN_MS, TIMEOUT_MAX_MS = 1, 1000


def load_config(path: Path = CFG_PATH) -> dict:
    parser = configparser.ConfigParser(inline_comment_prefixes=None)
    parser.read_string(path.read_text(encoding="utf-8"))
    cfg = {"timeout_ms": int(parser["stack_a"]["timeout_ms"])}
    if not TIMEOUT_MIN_MS <= cfg["timeout_ms"] <= TIMEOUT_MAX_MS:
        raise ValueError("config/gateway.cfg: timeout_ms 1..1000")
    return cfg
