# =============================================================================
# HYDRA-UMC - tools/i2c_plan.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Reference of the TIMINGR word of an I2C peripheral of the H7 (the C twin is src/mcu_stm32h757/Common/i2c_plan.h) and the reader of config/i2c4.cfg. Integer arithmetic in
picoseconds, step for step the same as the C code. Evidence: simulator."""
from __future__ import annotations

import configparser
from pathlib import Path

CFG_PATH = Path(__file__).resolve().parents[1] / "config" / "i2c4.cfg"

LIMITS = {
    "kernel_hz": (1_000_000, 200_000_000),
    "speed_hz": (10_000, 400_000),
    "digital_filter": (0, 15),
}
STANDARD_MAX_HZ = 100_000
RISE_MAX_NS = {"standard": 1000, "fast": 300}
FALL_MAX_NS = 300
AF_MIN_NS, AF_MAX_NS = 50, 260
SPEC = {  # ns: low, high, data set-up, longest time to valid data
    "standard": {"low": 4700, "high": 4000, "setup": 250, "valid_max": 3450},
    "fast": {"low": 1300, "high": 600, "setup": 100, "valid_max": 900},
}


def load_config(path: Path = CFG_PATH) -> dict:
    parser = configparser.ConfigParser(inline_comment_prefixes=None)
    parser.read_string(path.read_text(encoding="utf-8"))
    bus, filters = parser["bus"], parser["filters"]
    cfg = {key: int(bus[key]) for key in ("kernel_hz", "speed_hz", "rise_ns", "fall_ns")}
    cfg["analog_filter"] = int(filters["analog"])
    cfg["digital_filter"] = int(filters["digital"])
    check(cfg)
    return cfg


def mode_of(cfg: dict) -> str:
    return "fast" if cfg["speed_hz"] > STANDARD_MAX_HZ else "standard"


def check(cfg: dict) -> None:
    for key, (low, high) in LIMITS.items():
        if not low <= cfg[key] <= high:
            raise ValueError(f"{key} {low}..{high}")
    if not 1 <= cfg["rise_ns"] <= RISE_MAX_NS[mode_of(cfg)]:
        raise ValueError("rise_ns out of the range of the mode")
    if not 1 <= cfg["fall_ns"] <= FALL_MAX_NS:
        raise ValueError("fall_ns 1..300")
    if cfg["analog_filter"] not in (0, 1):
        raise ValueError("analog is 0 or 1")


def ceil_div(a: int, b: int) -> int:
    return -(-a // b)


def make_plan(cfg: dict) -> dict:
    """The smallest prescaler that fits the specification; ValueError when none does."""
    check(cfg)
    spec = SPEC[mode_of(cfg)]
    clk = 10**12 // cfg["kernel_hz"]
    dnf = cfg["digital_filter"]
    af_min = AF_MIN_NS if cfg["analog_filter"] else 0
    af_max = AF_MAX_NS if cfg["analog_filter"] else 0
    rise, fall = cfg["rise_ns"], cfg["fall_ns"]
    scldel_min = (rise + spec["setup"]) * 1000
    hold_cost = max(fall * 1000 - af_min * 1000 - (dnf + 3) * clk, 0)
    fixed_max = (rise + af_max) * 1000 + (dnf + 4) * clk
    if spec["valid_max"] * 1000 <= fixed_max:
        raise ValueError("no timing")
    sdadel_max = spec["valid_max"] * 1000 - fixed_max
    sync = (rise + fall + 2 * af_min) * 1000 + 2 * (dnf + 2) * clk
    period = ceil_div(10**12, cfg["speed_hz"])
    for presc in range(16):
        tp = (presc + 1) * clk
        scldel_steps = ceil_div(scldel_min, tp)
        sdadel = ceil_div(hold_cost, tp)
        if scldel_steps > 16 or sdadel > 15 or sdadel * tp > sdadel_max:
            continue
        low = ceil_div(spec["low"] * 1000, tp)
        high = ceil_div(spec["high"] * 1000, tp)
        steps = ceil_div(period - sync, tp)
        if steps < low + high:
            continue
        extra = steps - low - high
        scll = low + (extra - extra // 2)
        sclh = high + extra // 2
        if scll > 256 or sclh > 256:
            continue
        timingr = (presc << 28) | ((scldel_steps - 1) << 20) | (sdadel << 16) | ((sclh - 1) << 8) | (scll - 1)
        return {
            "timingr": timingr,
            "presc": presc,
            "scldel": scldel_steps - 1,
            "sdadel": sdadel,
            "sclh": sclh - 1,
            "scll": scll - 1,
            "actual_hz": 10**12 // ((scll + sclh) * tp + sync),
        }
    raise ValueError("no prescaler fits the specification")


BUSES_CFG_PATH = Path(__file__).resolve().parents[1] / "config" / "i2c_buses.cfg"
BUS_NAMES = ("i2c1", "i2c2", "i2c3")


def load_buses_config(path: Path = BUSES_CFG_PATH) -> dict:
    """config/i2c_buses.cfg: one section per bus with the keys of i2c4.cfg; returns {name: cfg} and refuses anything out of range or a bus that has no timing."""
    parser = configparser.ConfigParser(inline_comment_prefixes=None)
    parser.read_string(path.read_text(encoding="utf-8"))
    if tuple(parser.sections()) != BUS_NAMES:
        raise ValueError("config/i2c_buses.cfg: the sections are i2c1, i2c2 and i2c3")
    buses = {}
    for name in BUS_NAMES:
        section = parser[name]
        cfg = {key: int(section[key]) for key in ("kernel_hz", "speed_hz", "rise_ns", "fall_ns")}
        cfg["analog_filter"] = int(section["analog"])
        cfg["digital_filter"] = int(section["digital"])
        check(cfg)
        make_plan(cfg)
        buses[name] = cfg
    return buses
