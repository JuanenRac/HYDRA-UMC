# =============================================================================
# HYDRA-UMC - tools/fdcan_plan.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Reference of the numbers that FDCAN1 of the H757 board needs to be started on the stack bus (the C twin is src/mcu_stm32h757/Common/fdcan_plan.h) and the reader of
config/fdcan.cfg. The C driver (src/mcu_stm32h757/CM7/Fdcan1Start.c) writes the same numbers into the peripheral. Evidence: simulator."""
from __future__ import annotations

import configparser
from pathlib import Path

CFG_PATH = Path(__file__).resolve().parents[1] / "config" / "fdcan.cfg"

LIMITS = {
    "kernel_hz": (1_000_000, 200_000_000),
    "bit_rate_bps": (10_000, 1_000_000),
    "sample_permille": (500, 950),
    "max_quanta": (8, 128),
    "sjw": (1, 4),
    "rx_elements": (1, 32),
    "tx_elements": (1, 32),
}
QUANTA_MIN = 8
PRESCALER_MAX = 512
STACKA_FILTER = (0x600, 0x600)  # id, mask: the window 0x600..0x7FF of the controller boards
ESTOP_FILTER = (0x001, 0x7FF)

_SECTION = {"kernel_hz": "bus", "bit_rate_bps": "bus", "sample_permille": "bus", "max_quanta": "bus", "sjw": "bus", "rx_elements": "queues", "tx_elements": "queues"}


def load_config(path: Path = CFG_PATH) -> dict:
    parser = configparser.ConfigParser(inline_comment_prefixes=None)
    parser.read_string(path.read_text(encoding="utf-8"))
    cfg = {key: int(parser[section][key]) for key, section in _SECTION.items()}
    cfg["estop_filter"] = int(parser["filters"]["estop"])
    for key, (low, high) in LIMITS.items():
        if not low <= cfg[key] <= high:
            raise ValueError(f"config/fdcan.cfg: {key} {low}..{high}")
    if cfg["estop_filter"] not in (0, 1):
        raise ValueError("config/fdcan.cfg: estop is 0 or 1")
    return cfg


def make_plan(cfg: dict) -> dict:
    """The smallest prescaler whose bit has QUANTA_MIN..max_quanta quanta and gives exactly the bit rate; ValueError when there is none."""
    for key, (low, high) in LIMITS.items():
        if not low <= cfg[key] <= high:
            raise ValueError(f"{key} out of range")
    for prescaler in range(1, PRESCALER_MAX + 1):
        per_quantum = prescaler * cfg["bit_rate_bps"]
        quanta = cfg["kernel_hz"] // per_quantum
        if quanta < QUANTA_MIN:
            break
        if quanta > cfg["max_quanta"] or cfg["kernel_hz"] % per_quantum:
            continue
        seg1 = (quanta * cfg["sample_permille"] + 500) // 1000 - 1
        seg2 = quanta - 1 - seg1
        if seg2 < 1:
            seg2, seg1 = 1, quanta - 2
        filters = [STACKA_FILTER] + ([ESTOP_FILTER] if cfg["estop_filter"] else [])
        return {
            "prescaler": prescaler,
            "sjw": min(cfg["sjw"], seg2),
            "seg1": seg1,
            "seg2": seg2,
            "actual_bps": cfg["kernel_hz"] // (prescaler * quanta),
            "actual_sample_permille": (1 + seg1) * 1000 // quanta,
            "rx_elements": cfg["rx_elements"],
            "tx_elements": cfg["tx_elements"],
            "filters": filters,
        }
    raise ValueError("no prescaler gives exactly the bit rate")


ELEMENT_WORDS = 4
RAM_WORDS_TOTAL = 2560
FIELD_CFG_PATH = Path(__file__).resolve().parents[1] / "config" / "fdcan2.cfg"


def ram_words(plan: dict) -> int:
    return len(plan["filters"]) + (plan["rx_elements"] + plan["tx_elements"]) * ELEMENT_WORDS


def load_field_config(path: Path = FIELD_CFG_PATH) -> dict:
    """config/fdcan2.cfg: the bus and queue keys of fdcan.cfg and the one filter of the field bus."""
    parser = configparser.ConfigParser(inline_comment_prefixes=None)
    parser.read_string(path.read_text(encoding="utf-8"))
    cfg = {key: int(parser[section][key]) for key, section in _SECTION.items()}
    cfg["estop_filter"] = 0
    for key, (low, high) in LIMITS.items():
        if not low <= cfg[key] <= high:
            raise ValueError(f"config/fdcan2.cfg: {key} {low}..{high}")
    cfg["filter_id"] = int(parser["field"]["filter_id"], 0)
    cfg["filter_mask"] = int(parser["field"]["filter_mask"], 0)
    if not (0 <= cfg["filter_id"] <= 0x7FF and 0 <= cfg["filter_mask"] <= 0x7FF):
        raise ValueError("config/fdcan2.cfg: filter_id and filter_mask are 11 bits")
    return cfg


def make_field_plan(cfg: dict, first: dict) -> dict:
    """The plan of FDCAN2 placed after the plan of FDCAN1 in the message RAM; ValueError when it does not fit."""
    plan = make_plan({**cfg, "estop_filter": 0})
    plan["filters"] = [(cfg["filter_id"], cfg["filter_mask"])]
    plan["ram_offset"] = first.get("ram_offset", 0) + ram_words(first)
    if plan["ram_offset"] + ram_words(plan) > RAM_WORDS_TOTAL:
        raise ValueError("the second controller does not fit in the message RAM")
    return plan
