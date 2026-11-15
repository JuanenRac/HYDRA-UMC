# =============================================================================
# HYDRA-UMC - tools/dsi_plan.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Reference of the numbers of the DSI link of the H757 board (the C twin is src/mcu_stm32h757/Common/dsi_plan.h), the reader of config/panel.cfg and the maker of the C table
of the panel start-up (src/mcu_stm32h757/Common/panel_init_table.h).

    python tools/dsi_plan.py --write   rewrite the C table from config/panel.cfg
    python tools/dsi_plan.py --check   fail when the C table is not the one of config/panel.cfg

Evidence: simulator."""
from __future__ import annotations

import argparse
import configparser
import sys
from pathlib import Path

import display_plan

ROOT = Path(__file__).resolve().parents[1]
CFG_PATH = ROOT / "config" / "panel.cfg"
TABLE_PATH = ROOT / "src" / "mcu_stm32h757" / "Common" / "panel_init_table.h"

HSE_HZ = (4_000_000, 50_000_000)
PLL_IN_KHZ = (4000, 25000)
VCO_KHZ = (1_000_000, 2_000_000)
IDF_MAX = 7
NDIV = (10, 125)
ODFS = (1, 2, 4, 8)
LANE_KBPS = (62_500, 1_000_000)
TOLERANCE_MAX = 50
LANES_MAX = 2
BPPS = (16, 18, 24)
FPS = (10, 120)
HSA_HBP_MAX = 4095
HLINE_MAX = 32767
OVERHEAD_PERMILLE = 150
STEPS_MAX = 128
PARAMS_MAX = 64
DELAY_MAX_MS = 1000
RESET_MS = (1, 1000)
SLEEP_OUT, DISPLAY_ON, SLEEP_OUT_MIN_DELAY_MS = 0x11, 0x29, 120


def load_config(path: Path = CFG_PATH, display_path: Path = display_plan.CFG_PATH) -> dict:
    """The link configuration (with the picture of config/display.cfg), the reset timing and the init steps [(command, params, delay_ms)]."""
    parser = configparser.ConfigParser(inline_comment_prefixes=None)
    parser.read_string(path.read_text(encoding="utf-8"))
    picture = display_plan.load_config(display_path)
    link = {
        "hse_hz": int(parser["dsi"]["hse_hz"]),
        "lanes": int(parser["dsi"]["lanes"]),
        "lane_kbps": int(parser["dsi"]["lane_kbps"]),
        "tolerance_permille": int(parser["dsi"]["tolerance_permille"]),
        "bpp": int(parser["dsi"]["bpp"]),
        "fps": int(parser["dsi"]["fps"]),
        **{key: picture[key] for key in ("width", "height", "hsync", "hbp", "hfp", "vsync", "vbp", "vfp")},
    }
    reset = {"low_ms": int(parser["reset"]["low_ms"]), "high_ms": int(parser["reset"]["high_ms"])}
    steps = []
    for name, value in parser["init"].items():
        if ";" not in value:
            raise ValueError(f"config/panel.cfg: {name} needs 'bytes ; wait'")
        data, delay = value.split(";", 1)
        octets = [int(x, 16) for x in data.split()]
        if not octets or any(not 0 <= x <= 255 for x in octets):
            raise ValueError(f"config/panel.cfg: {name} needs a command byte and bytes")
        steps.append((octets[0], octets[1:], int(delay)))
    check_steps(steps)
    for key in ("low_ms", "high_ms"):
        if not RESET_MS[0] <= reset[key] <= RESET_MS[1]:
            raise ValueError("config/panel.cfg: the reset times are 1..1000 ms")
    return {"link": link, "reset": reset, "steps": steps}


def check_steps(steps: list) -> None:
    if not 1 <= len(steps) <= STEPS_MAX:
        raise ValueError("the table has 1..128 steps")
    sleep_at = display_at = None
    for index, (command, params, delay) in enumerate(steps):
        if len(params) > PARAMS_MAX:
            raise ValueError("at most 64 parameters in a step")
        if not 0 <= delay <= DELAY_MAX_MS:
            raise ValueError("a wait is 0..1000 ms")
        if command == SLEEP_OUT and sleep_at is None:
            sleep_at = index
            if delay < SLEEP_OUT_MIN_DELAY_MS:
                raise ValueError("the wait after the sleep-out is at least 120 ms")
        if command == DISPLAY_ON and display_at is None:
            display_at = index
    if sleep_at is None or display_at is None or display_at < sleep_at:
        raise ValueError("the table needs a sleep-out and then a display-on")


def ceil_div(a: int, b: int) -> int:
    return -(-a // b)


def to_byte_clocks(pixels: int, byte_khz: int, pixel_khz: int) -> int:
    return (pixels * byte_khz + pixel_khz // 2) // pixel_khz


def make_plan(link: dict) -> dict:
    """The PLL divisors and the video times; ValueError with the reason when there are none."""
    if not HSE_HZ[0] <= link["hse_hz"] <= HSE_HZ[1]:
        raise ValueError("hse")
    if not 1 <= link["lanes"] <= LANES_MAX:
        raise ValueError("lanes")
    if not LANE_KBPS[0] <= link["lane_kbps"] <= LANE_KBPS[1]:
        raise ValueError("lane rate")
    if not 0 <= link["tolerance_permille"] <= TOLERANCE_MAX:
        raise ValueError("tolerance")
    if link["bpp"] not in BPPS:
        raise ValueError("bpp")
    picture_ok = FPS[0] <= link["fps"] <= FPS[1] and 1 <= link["width"] <= 2048 and 1 <= link["height"] <= 2048
    porches_ok = all(1 <= link[k] <= 1024 for k in ("hsync", "hbp", "hfp", "vsync", "vbp", "vfp"))
    if not (picture_ok and porches_ok):
        raise ValueError("picture")
    hse_khz = link["hse_hz"] // 1000
    best = None
    for idf in range(1, IDF_MAX + 1):
        if not PLL_IN_KHZ[0] <= hse_khz // idf <= PLL_IN_KHZ[1]:
            continue
        for odf in ODFS:
            for ndiv in range(NDIV[0], NDIV[1] + 1):
                vco = hse_khz * 2 * ndiv // idf
                if not VCO_KHZ[0] <= vco <= VCO_KHZ[1]:
                    continue
                rate = vco // (2 * odf)
                error = abs(rate - link["lane_kbps"])
                if error * 1000 > link["lane_kbps"] * link["tolerance_permille"]:
                    continue
                if best is None or error < best[0] or (error == best[0] and vco < best[1]):
                    best = (error, vco, idf, ndiv, odf, rate)
    if best is None:
        raise ValueError("no PLL divisors")
    _, vco, idf, ndiv, odf, rate = best
    htotal = link["width"] + link["hsync"] + link["hbp"] + link["hfp"]
    vtotal = link["height"] + link["vsync"] + link["vbp"] + link["vfp"]
    pixel_khz = (htotal * vtotal * link["fps"] + 500) // 1000
    need = pixel_khz * link["bpp"] * (1000 + OVERHEAD_PERMILLE) // 1000
    if need > rate * link["lanes"]:
        raise ValueError("bandwidth")
    byte_khz = rate // 8
    hsa = to_byte_clocks(link["hsync"], byte_khz, pixel_khz)
    hbp = to_byte_clocks(link["hbp"], byte_khz, pixel_khz)
    hline = to_byte_clocks(htotal, byte_khz, pixel_khz)
    if hsa > HSA_HBP_MAX or hbp > HSA_HBP_MAX or hline > HLINE_MAX:
        raise ValueError("register")
    return {
        "idf": idf, "ndiv": ndiv, "odf": odf, "vco_khz": vco, "lane_kbps": rate, "byte_clock_khz": byte_khz, "pixel_khz": pixel_khz,
        "hsa": hsa, "hbp": hbp, "hline": hline, "vsa": link["vsync"], "vbp": link["vbp"], "vfp": link["vfp"], "vact": link["height"], "need_kbps": need,
    }


def render_table(data: dict) -> str:
    """The C text of Common/panel_init_table.h for the init steps and the reset times."""
    lines = [
        "/* =============================================================================",
        " * HYDRA-UMC Firmware - src/mcu_stm32h757/Common/panel_init_table.h",
        " * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>",
        " * GPL-3.0-or-later - see LICENSE",
        " * =============================================================================",
        " * The start-up table of the display panel, made from config/panel.cfg by tools/dsi_plan.py (python tools/dsi_plan.py --write). Do not edit it by hand: tests/test_dsi_plan.py",
        " * fails when it is not the table of the file.",
        " * ============================================================================= */",
        "#ifndef HYDRA_UMC_PANEL_INIT_TABLE_H",
        "#define HYDRA_UMC_PANEL_INIT_TABLE_H",
        "",
        '#include "panel_start.h"',
        "",
        "#define PANEL_INIT_RESET_LOW_MS %du" % data["reset"]["low_ms"],
        "#define PANEL_INIT_RESET_HIGH_MS %du" % data["reset"]["high_ms"],
        "",
    ]
    for index, (_, params, _) in enumerate(data["steps"]):
        if params:
            lines.append("static const uint8_t g_panel_params_%d[] = {%s};" % (index, ", ".join("0x%02X" % p for p in params)))
    lines += ["", "static const PanelStep g_panel_init[] = {"]
    for index, (command, params, delay) in enumerate(data["steps"]):
        pointer = "g_panel_params_%d" % index if params else "NULL"
        lines.append("    {0x%02X, %d, %s, %d}," % (command, len(params), pointer, delay))
    lines += ["};", "", "#define PANEL_INIT_STEPS %du" % len(data["steps"]), "", "#endif /* HYDRA_UMC_PANEL_INIT_TABLE_H */", ""]
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Check config/panel.cfg and make or check the C table of the panel start-up.")
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--write", action="store_true", help="rewrite src/mcu_stm32h757/Common/panel_init_table.h")
    group.add_argument("--check", action="store_true", help="fail when the C table is not the one of the file")
    args = parser.parse_args(argv)
    data = load_config()
    make_plan(data["link"])
    text = render_table(data)
    if args.write:
        TABLE_PATH.write_text(text, encoding="utf-8", newline="\n")
        print("PANEL_INIT_TABLE=WRITTEN")
        return 0
    if args.check:
        current = TABLE_PATH.read_text(encoding="utf-8").replace("\r\n", "\n") if TABLE_PATH.is_file() else ""
        if current != text:
            print("PANEL_INIT_TABLE=FAIL: run python tools/dsi_plan.py --write")
            return 1
        print("PANEL_INIT_TABLE=PASS")
        return 0
    print("PANEL_CFG=PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
