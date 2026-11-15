# =============================================================================
# HYDRA-UMC - tools/adc_plan.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Reference of what a reading of the ADC means (the C twin is src/mcu_stm32h757/Common/adc_plan.h) and the reader of config/adc.cfg. Integer arithmetic with rounding to the nearest,
step for step the same as the C code. Evidence: simulator."""
from __future__ import annotations

import configparser
from pathlib import Path

CFG_PATH = Path(__file__).resolve().parents[1] / "config" / "adc.cfg"

VREF_MV = (1800, 3600)
BITS = (12, 16)
SAMPLES = (1, 64)
TIMEOUT_MS = (1, 1000)
SAMPLE_CODES = {"16.5": 0, "64.5": 1, "387.5": 2}
ADC_MAX, CHANNEL_MAX = 3, 19
RATIO_MAX = 1_000_000
OFFSET_MAX_MV = 100_000
INT32_MAX = 2**31 - 1


def check_config(cfg: dict) -> None:
    if not VREF_MV[0] <= cfg["vref_mv"] <= VREF_MV[1]:
        raise ValueError("vref_mv 1800..3600")
    if cfg["bits"] not in BITS:
        raise ValueError("bits is 12 or 16")
    if not SAMPLES[0] <= cfg["samples"] <= SAMPLES[1]:
        raise ValueError("samples 1..64")
    if not TIMEOUT_MS[0] <= cfg["timeout_ms"] <= TIMEOUT_MS[1]:
        raise ValueError("timeout_ms 1..1000")
    if cfg["sample_code"] not in SAMPLE_CODES.values():
        raise ValueError("sample_code 0..2")


def check_channel(ch: dict) -> None:
    if not 1 <= ch["adc"] <= ADC_MAX or not 0 <= ch["channel"] <= CHANNEL_MAX:
        raise ValueError("adc 1..3, channel 0..19")
    if not 1 <= ch["num"] <= RATIO_MAX or not 1 <= ch["den"] <= RATIO_MAX:
        raise ValueError("ratio 1..1000000")
    if abs(ch["offset_mv"]) > OFFSET_MAX_MV:
        raise ValueError("offset +-100000 mV")


def load_config(path: Path = CFG_PATH) -> dict:
    """{"adc": {...}, "channels": {name: {...}}}; an input with adc 0 is kept (not yet assigned) and refused by to_millivolts."""
    parser = configparser.ConfigParser(inline_comment_prefixes=None)
    parser.optionxform = str
    parser.read_string(path.read_text(encoding="utf-8"))
    section = parser["adc"]
    if section["sample_time"].strip() not in SAMPLE_CODES:
        raise ValueError("sample_time is 16.5, 64.5 or 387.5")
    adc = {
        "vref_mv": int(section["vref_mv"]),
        "bits": int(section["bits"]),
        "samples": int(section["samples"]),
        "timeout_ms": int(section["timeout_ms"]),
        "sample_code": SAMPLE_CODES[section["sample_time"].strip()],
    }
    check_config(adc)
    channels = {}
    for name, value in parser["channels"].items():
        fields = [int(x) for x in value.split(",")]
        if len(fields) != 5:
            raise ValueError(f"channel {name}: adc, channel, num, den, offset_mv")
        channel = dict(zip(("adc", "channel", "num", "den", "offset_mv"), fields, strict=True))
        if channel["adc"] != 0:
            check_channel(channel)
        channels[name] = channel
    return {"adc": adc, "channels": channels}


def average(total: int, count: int) -> int:
    return (total + count // 2) // count


def to_millivolts(cfg: dict, ch: dict, raw: int) -> int:
    check_config(cfg)
    check_channel(ch)
    full = (1 << cfg["bits"]) - 1
    if raw > full:
        raise ValueError("raw over the resolution")
    pin_mv = (raw * cfg["vref_mv"] + full // 2) // full
    signal = (pin_mv * ch["num"] + ch["den"] // 2) // ch["den"] + ch["offset_mv"]
    if signal > INT32_MAX:
        raise ValueError("the result does not fit 32 bits")
    return signal
