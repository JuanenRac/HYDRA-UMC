# =============================================================================
# HYDRA-UMC - tools/usb_host_plan.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Reference of the numbers of the USB host of the H757 board (the C twin is src/mcu_stm32h757/Common/usb_host_plan.h) and the reader of config/usb_host.cfg. Evidence: simulator."""
from __future__ import annotations

import configparser
from pathlib import Path

CFG_PATH = Path(__file__).resolve().parents[1] / "config" / "usb_host.cfg"

CHANNELS = (1, 12)
IRQ_PRIORITY_MAX = 15
FIFO_TOTAL_WORDS = 320
FIFO_WORDS = {"rx": 128, "nptx": 96, "ptx": 64}
RX_PACKET_WORDS = 17


def check(cfg: dict) -> None:
    if not CHANNELS[0] <= cfg["host_channels"] <= CHANNELS[1]:
        raise ValueError("host_channels 1..12")
    if cfg["vbus_sensing"] not in (0, 1) or cfg["sof_output"] not in (0, 1):
        raise ValueError("vbus_sensing and sof_output are 0 or 1")
    if not 0 <= cfg["irq_priority"] <= IRQ_PRIORITY_MAX:
        raise ValueError("priority 0..15")


def load_config(path: Path = CFG_PATH) -> dict:
    parser = configparser.ConfigParser(inline_comment_prefixes=None)
    parser.read_string(path.read_text(encoding="utf-8"))
    cfg = {
        "host_channels": int(parser["host"]["host_channels"]),
        "vbus_sensing": int(parser["host"]["vbus_sensing"]),
        "sof_output": int(parser["host"]["sof_output"]),
        "irq_priority": int(parser["interrupt"]["priority"]),
    }
    check(cfg)
    return cfg


def fifo_words_used() -> int:
    return sum(FIFO_WORDS.values())
