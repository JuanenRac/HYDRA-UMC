# =============================================================================
# HYDRA-UMC - tools/dac_plan.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Reference of the code of the 12-bit DAC for the 0 to 10 V analog outputs (the C twin is src/mcu_stm32h757/Common/dac_plan.h) and the reader of config/dac.cfg. Integer arithmetic
with rounding to the nearest, step for step the same as the C code. Evidence: simulator."""
from __future__ import annotations

import configparser
from pathlib import Path

CFG_PATH = Path(__file__).resolve().parents[1] / "config" / "dac.cfg"

VREF_MV = (1800, 3600)
RATIO_MAX = 1000
OUT_MAX_MV = 10_000
CODE_MAX = 4095
CHANNELS = 2


def check(cfg: dict) -> None:
    if not VREF_MV[0] <= cfg["vref_mv"] <= VREF_MV[1]:
        raise ValueError("vref_mv 1800..3600")
    if not 1 <= cfg["gain_num"] <= RATIO_MAX or not 1 <= cfg["gain_den"] <= RATIO_MAX:
        raise ValueError("gain_num and gain_den 1..1000")
    if len(cfg["ceiling_mv"]) != CHANNELS or any(not 0 <= c <= OUT_MAX_MV for c in cfg["ceiling_mv"]):
        raise ValueError("ceiling 0..10000 mV for each output")


def load_config(path: Path = CFG_PATH) -> dict:
    parser = configparser.ConfigParser(inline_comment_prefixes=None)
    parser.read_string(path.read_text(encoding="utf-8"))
    cfg = {
        "vref_mv": int(parser["dac"]["vref_mv"]),
        "gain_num": int(parser["dac"]["gain_num"]),
        "gain_den": int(parser["dac"]["gain_den"]),
        "ceiling_mv": [int(parser["ceiling"]["ao1_mv"]), int(parser["ceiling"]["ao2_mv"])],
    }
    check(cfg)
    return cfg


def code_for(cfg: dict, channel: int, out_mv: int) -> int:
    check(cfg)
    if channel not in (1, 2):
        raise ValueError("output 1 or 2")
    if out_mv > cfg["ceiling_mv"][channel - 1]:
        raise ValueError("over the ceiling")
    divisor = cfg["vref_mv"] * cfg["gain_num"]
    value = (out_mv * cfg["gain_den"] * CODE_MAX + divisor // 2) // divisor
    if value > CODE_MAX:
        raise ValueError("the converter would need a code over 4095")
    return value
