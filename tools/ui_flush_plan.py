# =============================================================================
# HYDRA-UMC - tools/ui_flush_plan.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Reference of the flush plan of the interface library (the C twin is src/mcu_stm32h757/Common/ui_flush_plan.h) and the reader of config/ui_flush.cfg. Evidence: simulator."""
from __future__ import annotations

import configparser
from pathlib import Path

import display_plan

CFG_PATH = Path(__file__).resolve().parents[1] / "config" / "ui_flush.cfg"
BUFFER_KB = (4, 2048)
BUFFERS_MAX = 2


def load_config(path: Path = CFG_PATH) -> dict:
    parser = configparser.ConfigParser(inline_comment_prefixes=None)
    parser.read_string(path.read_text(encoding="utf-8"))
    cfg = {"buffer_kb": int(parser["buffer"]["size_kb"]), "buffers": int(parser["buffer"]["count"])}
    check(cfg)
    return cfg


def check(cfg: dict) -> None:
    if not BUFFER_KB[0] <= cfg["buffer_kb"] <= BUFFER_KB[1] or cfg["buffers"] not in (1, BUFFERS_MAX):
        raise ValueError("config/ui_flush.cfg: size_kb 4..2048 and count 1 or 2")


def make_plan(panel: dict, cfg: dict) -> dict:
    """The lines in a render buffer and the strips of the picture; ValueError when the buffer holds no line."""
    check(cfg)
    line_bytes = panel["width"] * display_plan.BYTES_PER_PIXEL[panel["format"]]
    lines = min(cfg["buffer_kb"] * 1024 // line_bytes, panel["height"])
    if lines < 1:
        raise ValueError("the buffer does not hold a single line")
    return {
        "lines": lines,
        "buffer_bytes": lines * line_bytes,
        "buffers": cfg["buffers"],
        "strips": -(-panel["height"] // lines),
        "total_bytes": lines * line_bytes * cfg["buffers"],
    }


def strips(panel: dict, plan: dict) -> list[tuple[int, int]]:
    """[(y, height)] of the bands, the last one the short one."""
    return [(i * plan["lines"], min(plan["lines"], panel["height"] - i * plan["lines"])) for i in range(plan["strips"])]


def area_to_rect(panel: dict, x1: int, y1: int, x2: int, y2: int) -> tuple[int, int, int, int]:
    """An area in the inclusive coordinates of the library as (x, y, w, h); ValueError when it is negative, inverted or outside the picture."""
    if x1 < 0 or y1 < 0 or x2 < x1 or y2 < y1 or x2 >= panel["width"] or y2 >= panel["height"]:
        raise ValueError("area")
    return (x1, y1, x2 - x1 + 1, y2 - y1 + 1)
