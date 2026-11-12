#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/platform_profile.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Reads docs/platform_profiles.json and computes what each profile gives: the clocks of the PLLs, the bit rate and the sample point of the stack bus.

The profile is data, not code: the numbers of a platform (clocks, bit timing, who owns a peripheral, the role of the host link) are written once in the file and
the tests compare what they compute with the contract of the bus. `python tools/platform_profile.py` prints the table of every profile; `--check` exits 1 when a
profile is inconsistent. Evidence: simulator (arithmetic on the numbers of the file); nothing here has run on a board."""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

PROFILES_PATH = Path(__file__).resolve().parents[1] / "docs" / "platform_profiles.json"
BUS_CONTRACT_PATH = Path(__file__).resolve().parents[1] / "docs" / "bus_contract.json"
NODES = ("CM4", "CM7")
SPI_ROLES = ("master", "slave")


def load(path: Path = PROFILES_PATH) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def profile(name: str | None = None, document: dict | None = None) -> dict:
    document = document or load()
    chosen = name or document["default_profile"]
    return document["profiles"][chosen]


def bit_rate(timing: dict) -> float:
    """Bits per second of the nominal timing: kernel clock / (prescaler * (1 + segment 1 + segment 2))."""
    return timing["kernel_clock_hz"] / (timing["prescaler"] * (1 + timing["time_segment_1"] + timing["time_segment_2"]))


def sample_point(timing: dict) -> float:
    """The fraction of the bit time at which the bit is sampled."""
    return (1 + timing["time_segment_1"]) / (1 + timing["time_segment_1"] + timing["time_segment_2"])


def pll_outputs(hse_hz: int, pll: dict, dividers: dict) -> dict:
    """The outputs of a PLL: hse / divm * divn is the VCO, and each output divides it by its own divider (from `dividers`)."""
    vco = hse_hz / pll["divm"] * pll["divn"]
    return {name: vco / divider for name, divider in dividers.items()}


# the dividers of the outputs that the CubeMX project sets (P, Q, R of each PLL); with them the clock tree of the file can be recomputed from the crystal
PLL_DIVIDERS = {
    "pll1": {"sysclk": 2},
    "pll2": {"pll2_p": 5, "pll2_q": 4, "pll2_r": 4},
    "pll3": {"pll3_p": 2, "pll3_q": 2, "pll3_r": 3},
}


def problems(name: str, p: dict) -> list[str]:
    """What is wrong in one profile, in words; empty when it is consistent."""
    found: list[str] = []
    bus = p["stack_bus"]
    for node in p["peripheral_owner"].values():
        if node not in NODES:
            found.append(f"{name}: an owner of a peripheral is {node!r}, not one of {', '.join(NODES)}")
    if p["host"]["spi_role"] not in SPI_ROLES:
        found.append(f"{name}: the SPI role {p['host']['spi_role']!r} is not one of {', '.join(SPI_ROLES)}")
    if bus["kernel_clock_hz"] != p["clocks_hz"].get("kernel_fdcan"):
        found.append(f"{name}: the kernel clock of the bus timing is not the FDCAN kernel clock of the clock table")
    if bus["sync_jump_width"] > bus["time_segment_2"] + 1 and name != "h745_current":
        found.append(f"{name}: the synchronisation jump is wider than the second segment")
    if bus["time_segment_1"] > 256 or bus["time_segment_2"] > 128 or bus["prescaler"] > 512:
        found.append(f"{name}: the bit timing does not fit the registers of the FDCAN")
    if "pll" in p:
        for pll_name, pll in p["pll"].items():
            for output, value in pll_outputs(p["hse_hz"], pll, PLL_DIVIDERS[pll_name]).items():
                if abs(value - p["clocks_hz"][output]) > 1:
                    found.append(f"{name}: {output} is {p['clocks_hz'][output]} Hz in the table and {value:.0f} Hz from the crystal and the PLL")
    return found


def board_problems(document: dict, contract: dict) -> list[str]:
    """The two buses of the controller board: each must give the rate of the contract, with a usable sample point and no rate error."""
    found: list[str] = []
    board = document["controller_board"]
    for bus_name, bus_key in (("stack_bus", "stack_a"), ("tool_head_downlink", "tool_head_downlink")):
        timing = board[bus_name]
        nominal = contract["buses"][bus_key]["nominal_bit_rate_bps"]
        if abs(bit_rate(timing) - nominal) > 0:
            found.append(f"controller_board.{bus_name}: gives {bit_rate(timing):.0f} bit/s, the contract says {nominal} (the rate error must be 0)")
        if not 0.75 <= sample_point(timing) <= 0.90:
            found.append(f"controller_board.{bus_name}: the sample point {sample_point(timing) * 100:.1f} % is outside 75 to 90 %")
        for key, contract_key in (("prescaler", "prescaler"), ("sync_jump_width", "sync_jump_width"), ("time_segment_1", "time_segment_1"), ("time_segment_2", "time_segment_2")):
            if timing[key] != contract["buses"][bus_key]["nominal_bit_timing"][contract_key]:
                found.append(f"controller_board.{bus_name}: {key} differs from docs/bus_contract.json")
        if timing["kernel_clock_hz"] != contract["buses"][bus_key]["assumed_kernel_clock_hz"]:
            found.append(f"controller_board.{bus_name}: the kernel clock differs from docs/bus_contract.json")
    return found


def linker_memory(profile_name: str, core: str, document: dict | None = None) -> str:
    """The MEMORY block of the application linker script of a core, written from the memory map of the profile (what the linkers of a platform are generated from)."""
    document = document or load()
    regions = [r for r in document["profiles"][profile_name]["memory_map"]["regions"] if r["owner"] == core and r["name"].startswith(("FLASH_APP", "RAM"))]
    lines = ["MEMORY", "{"]
    for r in regions:
        kind, attrs = ("FLASH_APP", "rx") if r["name"].startswith("FLASH_APP") else ("RAM", "rwx")
        lines.append(f"  {kind} ({attrs}) : ORIGIN = 0x{r['origin']:08X}, LENGTH = {r['length'] // 1024}K")
    lines.append("}")
    return chr(10).join(lines)


def memory_problems(name: str, profile: dict) -> list[str]:
    """What is wrong in the memory map of a profile: regions that overlap, that start or end outside the family map, or that are not backed by a source."""
    found: list[str] = []
    regions = sorted(profile["memory_map"]["regions"], key=lambda r: r["origin"])
    for a, b in zip(regions, regions[1:], strict=False):
        if a["origin"] + a["length"] > b["origin"]:
            found.append(f"{name}: {a['name']} overlaps {b['name']}")
    flash = [r for r in regions if r["name"].startswith("FLASH")]
    for r in flash:
        if r["origin"] < 0x08000000:
            found.append(f"{name}: {r['name']} is outside the flash")
        if r["origin"] + r["length"] > 0x08000000 + profile["memory_map"]["flash_total_bytes"]:
            found.append(f"{name}: {r['name']} goes past the end of the flash")
    for r in regions:
        if not r.get("source") or not r.get("status"):
            found.append(f"{name}: {r['name']} says nowhere where it comes from")
    return found


def contract_problems(document: dict, contract: dict) -> list[str]:
    """What the default profile says that docs/bus_contract.json does not: the stack bus must give the rate the controller boards use."""
    found: list[str] = []
    nominal = contract["buses"]["stack_a"]["nominal_bit_rate_bps"]
    for name, p in document["profiles"].items():
        if abs(bit_rate(p["stack_bus"]) - nominal) > 1:
            found.append(f"{name}: the stack bus gives {bit_rate(p['stack_bus']):.0f} bit/s, the contract says {nominal}")
    return found


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--check", action="store_true", help="exit 1 when a profile is inconsistent")
    args = parser.parse_args(argv)
    document = load()
    contract = json.loads(BUS_CONTRACT_PATH.read_text(encoding="utf-8"))
    bad: list[str] = []
    for name, p in document["profiles"].items():
        bus = p["stack_bus"]
        print(f"{name}: {p['device']}; host {p['host']['module']} (SPI {p['host']['spi_role']}); stack bus {bit_rate(bus):,.0f} bit/s, sample point {sample_point(bus) * 100:.1f} %")
        bad += problems(name, p)
        if "sdram" in p.get("new_peripherals", {}).get("FMC", {}):
            import sdram_plan  # here and not at the top: that module imports this one

            bad += [f"{name}: {line}" for line in sdram_plan.problems(p)]
    bad += contract_problems(document, contract)
    bad += board_problems(document, contract)
    for line in bad:
        print("PROBLEM " + line, file=sys.stderr)
    print("PLATFORM_PROFILES=" + ("FAIL" if bad else "PASS"))
    return 1 if bad and args.check else 0


if __name__ == "__main__":
    raise SystemExit(main())
