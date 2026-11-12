#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/sdram_plan.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Plans the start-up of the external SDRAM of the STM32H757 board: the registers of the FMC and the JEDEC sequence, computed from docs/platform_profiles.json.

The board has SDR SDRAM on the FMC (two banks, each a pair of 16-bit chips as 32 bits). Nothing may use it before the JEDEC sequence: clock enable, a wait of at least
100 us, precharge all, auto-refresh cycles, load of the mode register and the refresh rate. This module turns the numbers of the profile (geometry, CAS latency, the timings
in SDRAM clock cycles, the refresh period) into the values of the registers SDCR, SDTR, SDCMR and SDRTR and into the ordered list of steps; `src/mcu_stm32h757/Common/
sdram_init.h` does the same arithmetic in C and its host test compares it with `tests/sdram_plan_golden.h`, which `tools/gen_sdram_golden.py` writes from this module.

The size of each bank is a setting of the profile (`size_bytes`, the window the firmware uses) and a bank can be switched off; the plan refuses a size the geometry cannot
hold. Evidence: simulator (arithmetic and a model of the sequence); the values are starting values from the CubeMX project and nothing here has run on a board."""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import platform_profile as pp

MODE_CLOCK_ENABLE, MODE_PALL, MODE_AUTO_REFRESH, MODE_LOAD_MODE = 1, 2, 3, 4
# SDCR fields
CAS_VALUES = (1, 2, 3)
SDCLK_PERIODS = (2, 3)
BUS_WIDTHS = {8: 0, 16: 1, 32: 2}
BANK_TARGET_BITS = {1: 1 << 4, 2: 1 << 3}  # SDCMR CTB1 (bit 4) and CTB2 (bit 3)
MODE_REGISTER_BURST_1 = 0x0000
MODE_REGISTER_SEQUENTIAL = 0x0000
MODE_REGISTER_WRITE_SINGLE = 0x0200
MODE_REGISTER_CAS_SHIFT = 4
# bank 2 takes the SDRAM clock period, the read burst, the read pipe delay, tRC and tRP from the registers of bank 1 (as the HAL of the family does): only the geometry,
# the CAS latency and the other timings are written to its own registers
SDCR2_MASK = 0x3FF
SDTR2_MASK = 0xFFFFFFFF & ~((0xF << 12) | (0xF << 20))
TIMINGS = ("tmrd", "txsr", "tras", "trc", "twr", "trp", "trcd")
TIMING_SHIFT = {"tmrd": 0, "txsr": 4, "tras": 8, "trc": 12, "twr": 16, "trp": 20, "trcd": 24}


def sdram(profile: dict) -> dict:
    return profile["new_peripherals"]["FMC"]["sdram"]


def capacity_bytes(cfg: dict) -> int:
    """What the geometry holds: 2^(rows + columns) words in each of the internal banks, a word being the width of the bus."""
    return (1 << (cfg["row_bits"] + cfg["column_bits"])) * cfg["internal_banks"] * cfg["bus_width_bits"] // 8


def sdclk_hz(profile: dict) -> float:
    return profile["clocks_hz"]["hclk"] / sdram(profile)["sdclk_period_hclk"]


def problems(profile: dict) -> list[str]:
    cfg = sdram(profile)
    found: list[str] = []
    if cfg["bus_width_bits"] not in BUS_WIDTHS:
        found.append(f"bus width {cfg['bus_width_bits']} is not 8, 16 or 32 bits")
    if not 11 <= cfg["row_bits"] <= 13:
        found.append("rows must be 11 to 13 bits")
    if not 8 <= cfg["column_bits"] <= 11:
        found.append("columns must be 8 to 11 bits")
    if cfg["internal_banks"] not in (2, 4):
        found.append("the SDRAM has 2 or 4 internal banks")
    if cfg["cas_latency"] not in CAS_VALUES:
        found.append("CAS latency must be 1, 2 or 3")
    if cfg["sdclk_period_hclk"] not in SDCLK_PERIODS:
        found.append("the SDRAM clock is 2 or 3 times the period of HCLK")
    for name in TIMINGS:
        if not 1 <= cfg["timing_sdclk_cycles"].get(name, 0) <= 16:
            found.append(f"timing {name} must be 1 to 16 clock cycles")
    if cfg["timing_sdclk_cycles"]["tras"] > cfg["timing_sdclk_cycles"]["trc"]:
        found.append("tRAS cannot be longer than tRC")
    if cfg["auto_refresh_cycles"] < 2 or cfg["auto_refresh_cycles"] > 16:
        found.append("at least 2 and at most 16 auto-refresh cycles")
    if cfg["startup_delay_us"] < 100:
        found.append("the JEDEC start-up wait is at least 100 us")
    capacity = capacity_bytes(cfg)
    seen = set()
    regions = {r["name"]: r for r in profile["memory_map"]["regions"]}
    for bank in cfg["banks"]:
        if bank["fmc_bank"] in seen:
            found.append(f"FMC bank {bank['fmc_bank']} listed twice")
        seen.add(bank["fmc_bank"])
        if bank["fmc_bank"] not in BANK_TARGET_BITS:
            found.append(f"FMC bank {bank['fmc_bank']} is not an SDRAM bank (1 or 2)")
        if bank["enabled"]:
            if bank["size_bytes"] <= 0 or bank["size_bytes"] > capacity:
                found.append(f"{bank['region']}: {bank['size_bytes']} bytes do not fit the {capacity} bytes the geometry holds")
            if bank["size_bytes"] % 4096:
                found.append(f"{bank['region']}: the size is not a multiple of 4096 bytes")
            region = regions.get(bank["region"])
            if region is None:
                found.append(f"{bank['region']} has no region in the memory map")
            elif region["length"] != bank["size_bytes"]:
                found.append(f"{bank['region']}: the region says {region['length']} bytes and the SDRAM settings {bank['size_bytes']}")
    if not any(b["enabled"] for b in cfg["banks"]):
        found.append("no SDRAM bank is enabled")
    if refresh_count(profile) < 1:
        found.append("the refresh period is shorter than the SDRAM clock can count")
    if refresh_count(profile) > 0x1FFF:
        found.append("the refresh count does not fit the 13 bits of SDRTR")
    return found


def sdcr(profile: dict) -> int:
    """SDCR: columns, rows, bus width, internal banks, CAS, SDRAM clock period, read burst and read pipe delay (write protection off)."""
    c = sdram(profile)
    value = c["column_bits"] - 8
    value |= (c["row_bits"] - 11) << 2
    value |= BUS_WIDTHS[c["bus_width_bits"]] << 4
    value |= (1 if c["internal_banks"] == 4 else 0) << 6
    value |= c["cas_latency"] << 7
    value |= c["sdclk_period_hclk"] << 10
    value |= (1 if c["read_burst"] else 0) << 12
    value |= c["read_pipe_delay_hclk"] << 13
    return value


def sdtr(profile: dict) -> int:
    """SDTR: each timing is held as (cycles - 1) in its own nibble."""
    t = sdram(profile)["timing_sdclk_cycles"]
    return sum((t[name] - 1) << TIMING_SHIFT[name] for name in TIMINGS)


def mode_register(profile: dict) -> int:
    """The mode register of the SDRAM: burst length 1, sequential, the CAS latency of the profile, single write."""
    return MODE_REGISTER_BURST_1 | MODE_REGISTER_SEQUENTIAL | (sdram(profile)["cas_latency"] << MODE_REGISTER_CAS_SHIFT) | MODE_REGISTER_WRITE_SINGLE


def refresh_count(profile: dict) -> int:
    """The refresh counter: the period of a row refresh (the refresh period divided by the rows) in SDRAM clock cycles, less the margin the reference manual advises."""
    c = sdram(profile)
    cycles = c["refresh_period_ms"] * 1e-3 / (1 << c["row_bits"]) * sdclk_hz(profile)
    return int(cycles) - c["refresh_margin_cycles"]


def target_bits(profile: dict) -> int:
    bits = 0
    for bank in sdram(profile)["banks"]:
        if bank["enabled"]:
            bits |= BANK_TARGET_BITS[bank["fmc_bank"]]
    return bits


def sdcmr(profile: dict, mode: int, rounds: int = 1, mode_register_value: int = 0) -> int:
    """SDCMR: the command, the banks it goes to, the number of auto-refresh cycles less one and the mode register to load."""
    return mode | target_bits(profile) | ((rounds - 1) << 5) | (mode_register_value << 9)


def sequence(profile: dict) -> list[dict]:
    """The ordered steps of the start-up: a register write or a wait. The registers of the banks are written first (SDCR of both banks, SDTR of both)."""
    c = sdram(profile)

    def cmd(mode: int, rounds: int = 1, mrd: int = 0) -> dict:
        return {"op": "write", "reg": "SDCMR", "value": sdcmr(profile, mode, rounds, mrd)}

    return [
        {"op": "write", "reg": "SDCR1", "value": sdcr(profile)},
        {"op": "write", "reg": "SDTR1", "value": sdtr(profile)},
        {"op": "write", "reg": "SDCR2", "value": sdcr(profile) & SDCR2_MASK},
        {"op": "write", "reg": "SDTR2", "value": sdtr(profile) & SDTR2_MASK},
        cmd(MODE_CLOCK_ENABLE),
        {"op": "wait_us", "value": c["startup_delay_us"]},
        cmd(MODE_PALL),
        cmd(MODE_AUTO_REFRESH, c["auto_refresh_cycles"]),
        cmd(MODE_LOAD_MODE, 1, mode_register(profile)),
        {"op": "write", "reg": "SDRTR", "value": refresh_count(profile) << 1},
    ]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--profile", default="h757_target")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args(argv)
    profile = pp.profile(args.profile)
    if "sdram" not in profile.get("new_peripherals", {}).get("FMC", {}):
        print(f"SDRAM_PLAN=SKIP {args.profile} has no SDRAM")
        return 0
    found = problems(profile)
    if found:
        for line in found:
            print(f"SDRAM_PLAN=FAIL {line}", file=sys.stderr)
        return 1
    if not args.check:
        for step in sequence(profile):
            print(step)
    print(f"SDRAM_PLAN=PASS {args.profile}: {sum(1 for b in sdram(profile)['banks'] if b['enabled'])} bank(s), {capacity_bytes(sdram(profile)) // (1 << 20)} MiB per bank, refresh count {refresh_count(profile)}")
    return 0


if __name__ == "__main__":
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    raise SystemExit(main())
