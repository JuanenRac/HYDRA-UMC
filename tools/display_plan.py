# =============================================================================
# HYDRA-UMC - tools/display_plan.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Reference of the numbers that the LTDC and the DMA2D of the H757 board need for the panel (the C twin is src/mcu_stm32h757/Common/display_plan.h) and the reader of
config/display.cfg. The C driver (src/mcu_stm32h757/CM7/DisplayLtdc.c) refuses the same limits. Evidence: simulator."""
from __future__ import annotations

import configparser
from pathlib import Path

CFG_PATH = Path(__file__).resolve().parents[1] / "config" / "display.cfg"

MAX_SIZE = 2048
MAX_SYNC = 1024
MAX_POLL = 1000000
BYTES_PER_PIXEL = {"rgb565": 2, "argb8888": 4}
LTDC_PF = {"rgb565": 2, "argb8888": 0}
DMA2D_CM = {"rgb565": 2, "argb8888": 0}
TIMING_KEYS = ("hsync", "hbp", "hfp", "vsync", "vbp", "vfp")


def load_config(path: Path = CFG_PATH) -> dict:
    parser = configparser.ConfigParser(inline_comment_prefixes=None)
    parser.read_string(path.read_text(encoding="utf-8"))
    panel, driver = parser["panel"], parser["driver"]
    cfg = {name: int(panel[name]) for name in ("width", "height") + TIMING_KEYS}
    cfg["format"] = panel["format"].strip()
    cfg["poll_limit"] = int(driver["poll_limit"])
    if not 1 <= cfg["width"] <= MAX_SIZE or not 1 <= cfg["height"] <= MAX_SIZE:
        raise ValueError("config/display.cfg: width and height 1..2048")
    if any(not 1 <= cfg[key] <= MAX_SYNC for key in TIMING_KEYS):
        raise ValueError("config/display.cfg: every sync and porch 1..1024")
    if cfg["format"] not in BYTES_PER_PIXEL:
        raise ValueError("config/display.cfg: format is rgb565 or argb8888")
    if not 1 <= cfg["poll_limit"] <= MAX_POLL:
        raise ValueError("config/display.cfg: poll_limit 1..1000000")
    return cfg


def plan(cfg: dict, fb_addr: int, window_bytes: int) -> dict:
    """The words of the LTDC timing and layer for `cfg` (what Display_PlanMake gives)."""
    bpp = BYTES_PER_PIXEL[cfg["format"]]
    pitch = cfg["width"] * bpp
    frame = pitch * cfg["height"]
    if fb_addr & 3 or frame > window_bytes or fb_addr + frame > 1 << 32:
        raise ValueError("the frame buffer does not fit its window")
    h_acc = cfg["hsync"] + cfg["hbp"]
    v_acc = cfg["vsync"] + cfg["vbp"]
    width, height = cfg["width"], cfg["height"]
    return {
        "sscr": ((cfg["hsync"] - 1) << 16) | (cfg["vsync"] - 1),
        "bpcr": ((h_acc - 1) << 16) | (v_acc - 1),
        "awcr": ((h_acc + width - 1) << 16) | (v_acc + height - 1),
        "twcr": ((h_acc + width + cfg["hfp"] - 1) << 16) | (v_acc + height + cfg["vfp"] - 1),
        "whpcr": ((h_acc + width - 1) << 16) | h_acc,
        "wvpcr": ((v_acc + height - 1) << 16) | v_acc,
        "pfcr": LTDC_PF[cfg["format"]],
        "cfblr": (pitch << 16) | (pitch + 3),
        "cfblnr": height,
        "cfbar": fb_addr,
        "pitch_bytes": pitch,
        "frame_bytes": frame,
    }
