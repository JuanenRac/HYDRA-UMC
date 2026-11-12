#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/rcc_plan.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Plans the clock set-up of the CM7 of the STM32H757 board: the values of the RCC registers that give the clock tree of docs/platform_profiles.json (profile h757_target).

The crystal is 25 MHz; PLL1 gives the core clock (400 MHz), PLL2 the 100 MHz kernel clock of the FDCAN of the stack bus; the second core runs at the bus clock (200 MHz). The
display clock (PLL3) is not set up here: the profile gives it a reference of 25 MHz, outside the 1 to 16 MHz the PLL accepts, which has to be confirmed with the board file before
that PLL is started. `src/mcu_stm32h757/Common/clock_config.h` does the same arithmetic in C and its host test compares it with `tests/rcc_plan_golden.h`, which
`tools/gen_rcc_golden.py` writes from this module.

The layout of the registers is that of the STM32H7 family as the HAL of the family uses it; it is still to be confirmed with the reference manual of the chip (the owner decided
that the memory map is confirmed without it, and the register layout is a separate question). Evidence: simulator (arithmetic); nothing here has run on a board."""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import platform_profile as pp

# RCC_CR bits
HSEON, HSERDY, PLL1ON, PLL1RDY, PLL2ON, PLL2RDY = 1 << 16, 1 << 17, 1 << 24, 1 << 25, 1 << 26, 1 << 27
SW_PLL1 = 3
FDCANSEL_PLL2_Q = 2
HPRE = {1: 0, 2: 8, 4: 9, 8: 10, 16: 11, 64: 12, 128: 13, 256: 14, 512: 15}
APB = {1: 0, 2: 4, 4: 5, 8: 6, 16: 7}
CPRE = HPRE


def rge(ref_hz: float) -> int:
    """The input range of a PLL reference: 1 to 2 MHz is 0, 2 to 4 is 1, 4 to 8 is 2, 8 to 16 is 3."""
    for code, limit in ((0, 2e6), (1, 4e6), (2, 8e6), (3, 16e6)):
        if ref_hz <= limit:
            return code
    raise ValueError("the PLL reference is above 16 MHz")


def vcosel(vco_hz: float) -> int:
    """0: wide range (192 to 836 MHz), 1: medium range (150 to 420 MHz)."""
    return 0 if vco_hz >= 192e6 else 1


def divisors(profile: dict) -> dict:
    """The AHB and APB dividers that give the clocks of the profile from the core clock."""
    c = profile["clocks_hz"]
    return {
        "cpre": c["sysclk"] // c["sysclk"],
        "hpre": c["sysclk"] // c["hclk"],
        "d1ppre": c["hclk"] // c["apb3"],
        "d2ppre1": c["hclk"] // c["apb1"],
        "d2ppre2": c["hclk"] // c["apb2"],
        "d3ppre": c["hclk"] // c["apb4"],
    }


def problems(profile: dict) -> list[str]:
    found: list[str] = []
    hse = profile["hse_hz"]
    for name in ("pll1", "pll2"):
        pll = profile["pll"][name]
        ref = hse / pll["divm"]
        if not 1e6 <= ref <= 16e6:
            found.append(f"{name}: the reference {ref:.0f} Hz is outside 1 to 16 MHz")
        vco = ref * pll["divn"]
        if not 150e6 <= vco <= 836e6:
            found.append(f"{name}: the VCO {vco:.0f} Hz is outside the ranges of the PLL")
    d = divisors(profile)
    if d["hpre"] not in HPRE or d["cpre"] not in CPRE:
        found.append("the AHB divider is not one the register has")
    for key in ("d1ppre", "d2ppre1", "d2ppre2", "d3ppre"):
        if d[key] not in APB:
            found.append(f"{key} {d[key]} is not a divider the register has")
    if profile["clocks_hz"]["cpu2"] != profile["clocks_hz"]["hclk"]:
        found.append("the second core is meant to run at the bus clock")
    if profile["clocks_hz"]["kernel_fdcan"] != profile["clocks_hz"]["pll2_q"]:
        found.append("the FDCAN kernel clock is meant to be PLL2_Q")
    return found


def pllckselr(profile: dict) -> int:
    """PLLCKSELR: the source is the crystal (2) and the input dividers of PLL1 and PLL2 (PLL3 is left at its reset value of 32, off)."""
    p = profile["pll"]
    return 2 | (p["pll1"]["divm"] << 4) | (p["pll2"]["divm"] << 12) | (32 << 20)


def pllcfgr(profile: dict) -> int:
    """PLLCFGR: input range and VCO range of PLL1 and PLL2, and the outputs that are used (PLL1 P; PLL2 P, Q, R); the fractional dividers stay off."""
    hse = profile["hse_hz"]
    value = 0
    for number, outputs in ((1, ("p",)), (2, ("p", "q", "r"))):
        pll = profile["pll"][f"pll{number}"]
        ref = hse / pll["divm"]
        base = 0 if number == 1 else 4
        value |= vcosel(ref * pll["divn"]) << (base + 1)
        value |= rge(ref) << (base + 2)
        first = 16 if number == 1 else 19
        for k, out in enumerate(("p", "q", "r")):
            if out in outputs:
                value |= 1 << (first + k)
    return value


def plldivr(profile: dict, number: int) -> int:
    """PLLnDIVR: N, P, Q, R each held as the value less one."""
    pll = profile["pll"][f"pll{number}"]
    if number == 1:
        p, q, r = pp.PLL_DIVIDERS["pll1"]["sysclk"], 1, 1
    else:
        d = pp.PLL_DIVIDERS["pll2"]
        p, q, r = d["pll2_p"], d["pll2_q"], d["pll2_r"]
    return (pll["divn"] - 1) | ((p - 1) << 9) | ((q - 1) << 16) | ((r - 1) << 24)


def d1cfgr(profile: dict) -> int:
    d = divisors(profile)
    return HPRE[d["hpre"]] | (APB[d["d1ppre"]] << 4) | (CPRE[d["cpre"]] << 8)


def d2cfgr(profile: dict) -> int:
    d = divisors(profile)
    return (APB[d["d2ppre1"]] << 4) | (APB[d["d2ppre2"]] << 8)


def d3cfgr(profile: dict) -> int:
    return APB[divisors(profile)["d3ppre"]] << 4


def d2ccip1r(profile: dict) -> int:
    """D2CCIP1R: the FDCAN kernel clock is PLL2_Q (field FDCANSEL, bits 29:28)."""
    return FDCANSEL_PLL2_Q << 28


def sequence(profile: dict) -> list[dict]:
    """The ordered steps of the set-up: register writes and the waits for a ready flag."""
    return [
        {"op": "flash_latency", "value": profile["clocks_hz"]["hclk"]},
        {"op": "set_bits", "reg": "CR", "value": HSEON},
        {"op": "wait_bits", "reg": "CR", "value": HSERDY},
        {"op": "write", "reg": "PLLCKSELR", "value": pllckselr(profile)},
        {"op": "write", "reg": "PLLCFGR", "value": pllcfgr(profile)},
        {"op": "write", "reg": "PLL1DIVR", "value": plldivr(profile, 1)},
        {"op": "write", "reg": "PLL2DIVR", "value": plldivr(profile, 2)},
        {"op": "set_bits", "reg": "CR", "value": PLL1ON | PLL2ON},
        {"op": "wait_bits", "reg": "CR", "value": PLL1RDY | PLL2RDY},
        {"op": "write", "reg": "D1CFGR", "value": d1cfgr(profile)},
        {"op": "write", "reg": "D2CFGR", "value": d2cfgr(profile)},
        {"op": "write", "reg": "D3CFGR", "value": d3cfgr(profile)},
        {"op": "write", "reg": "D2CCIP1R", "value": d2ccip1r(profile)},
        {"op": "write", "reg": "CFGR", "value": SW_PLL1},
        {"op": "wait_switch", "reg": "CFGR", "value": SW_PLL1},
    ]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--profile", default="h757_target")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args(argv)
    profile = pp.profile(args.profile)
    if "pll" not in profile:
        print(f"RCC_PLAN=SKIP {args.profile} has no clock tree")
        return 0
    found = problems(profile)
    for line in found:
        print(f"RCC_PLAN=FAIL {line}", file=sys.stderr)
    if found:
        return 1
    if not args.check:
        for step in sequence(profile):
            print(step)
    print(f"RCC_PLAN=PASS {args.profile}")
    return 0


if __name__ == "__main__":
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    raise SystemExit(main())
