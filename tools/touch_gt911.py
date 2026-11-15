# =============================================================================
# HYDRA-UMC - tools/touch_gt911.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Reference of the touch input of the interface (the C twin is src/mcu_stm32h757/Common/touch_gt911.h) and the reader of the [touch] section of config/lvgl.cfg: the mapping of a raw
position to the picture and the decoding of a frame of the Goodix GT911. Evidence: simulator."""
from __future__ import annotations

import configparser
from pathlib import Path

import display_plan

CFG_PATH = Path(__file__).resolve().parents[1] / "config" / "lvgl.cfg"
ADDRESSES = (0x5D, 0x14)
SIZE_MAX = 2048
MAX_POINTS = 5
POINT_BYTES = 8
POLL_MS = (5, 100)


def load_config(path: Path = CFG_PATH, panel: dict | None = None) -> dict:
    parser = configparser.ConfigParser(inline_comment_prefixes=None)
    parser.read_string(path.read_text(encoding="utf-8"))
    section = parser["touch"]
    if section["controller"].strip() != "gt911":
        raise ValueError("config/lvgl.cfg: only the gt911 is implemented")
    panel = panel or display_plan.load_config()
    cfg = {
        "address7": int(section["address"], 0),
        "raw_width": int(section["raw_width"]),
        "raw_height": int(section["raw_height"]),
        "width": panel["width"],
        "height": panel["height"],
        "swap_xy": int(section["swap_xy"]),
        "mirror_x": int(section["mirror_x"]),
        "mirror_y": int(section["mirror_y"]),
        "poll_ms": int(section["poll_ms"]),
    }
    check(cfg)
    return cfg


def check(cfg: dict) -> None:
    if cfg["address7"] not in ADDRESSES:
        raise ValueError("config/lvgl.cfg: the address of the GT911 is 0x5D or 0x14")
    for key in ("raw_width", "raw_height", "width", "height"):
        if not 1 <= cfg[key] <= SIZE_MAX:
            raise ValueError(f"config/lvgl.cfg: {key} outside 1..{SIZE_MAX}")
    if any(cfg[key] not in (0, 1) for key in ("swap_xy", "mirror_x", "mirror_y")):
        raise ValueError("config/lvgl.cfg: swap_xy, mirror_x and mirror_y are 0 or 1")
    if "poll_ms" in cfg and not POLL_MS[0] <= cfg["poll_ms"] <= POLL_MS[1]:
        raise ValueError("config/lvgl.cfg: poll_ms outside 5..100")


def map_point(cfg: dict, raw_x: int, raw_y: int) -> tuple[int, int]:
    """A raw position as a position of the picture: swap, mirror, scale (rounded down); ValueError when it is outside the raw size."""
    check(cfg)
    if not 0 <= raw_x < cfg["raw_width"] or not 0 <= raw_y < cfg["raw_height"]:
        raise ValueError("outside the raw size")
    sx, sy, rw, rh = raw_x, raw_y, cfg["raw_width"], cfg["raw_height"]
    if cfg["swap_xy"]:
        sx, sy, rw, rh = sy, sx, rh, rw
    if cfg["mirror_x"]:
        sx = rw - 1 - sx
    if cfg["mirror_y"]:
        sy = rh - 1 - sy
    return sx * cfg["width"] // rw, sy * cfg["height"] // rh


def decode_frame(status: int, point: bytes) -> dict | None:
    """The frame of the controller: None when no new frame is ready, else {'points': n, 'x': raw, 'y': raw} (the first point; x and y None when released). ValueError for more points than the chip has."""
    if not status & 0x80:
        return None
    count = status & 0x0F
    if count > MAX_POINTS:
        raise ValueError("more points than the controller has")
    if count == 0:
        return {"points": 0, "x": None, "y": None}
    return {"points": count, "x": point[1] | point[2] << 8, "y": point[3] | point[4] << 8}
