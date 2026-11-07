#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/renode_gap_report.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Reads the platform descriptions (.repl) of an unpacked Renode and says, for the two chips of this project, what Renode models and what it lacks.

The chips: STM32G474 (controller board) and STM32H745 (kinematic brain, two cores). For each family the report lists the platform files found
and, for the peripherals the bootloaders use (FDCAN, SPI, hardware semaphore HSEM, flash controller), whether a platform description declares
one. A chip with no platform file is reported as NOT SUPPORTED; nothing is invented and no substitute chip is assumed. Evidence: a listing of
files, not a run of the firmware.

    python tools/renode_gap_report.py RENODE_DIRECTORY [--markdown FILE]
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

CHIPS = {
    "STM32G474": re.compile(r"stm32g4", re.I),
    "STM32H745": re.compile(r"stm32h7", re.I),
}
PERIPHERALS = {
    "FDCAN": re.compile(r"\bfdcan\w*|\bCAN\w*\s*:", re.I),
    "SPI": re.compile(r"\bSPI\w*\s*:", re.I),
    "HSEM": re.compile(r"\bhsem\b|HardwareSemaphore", re.I),
    "Flash controller": re.compile(r"flash\w*\s*:|STM32\w*Flash", re.I),
}


def find_platforms(root: Path) -> list[Path]:
    return sorted(p for p in root.rglob("*.repl") if "platforms" in p.parts)


def analyse(platforms: list[Path]) -> dict[str, dict]:
    result: dict[str, dict] = {}
    for chip, pattern in CHIPS.items():
        files = [p for p in platforms if pattern.search(p.name)]
        found: dict[str, list[str]] = {name: [] for name in PERIPHERALS}
        for path in files:
            text = path.read_text(encoding="utf-8", errors="replace")
            for name, peripheral in PERIPHERALS.items():
                if peripheral.search(text):
                    found[name].append(path.name)
        result[chip] = {"files": [p.name for p in files], "peripherals": found}
    return result


def render(report: dict[str, dict], version: str = "") -> str:
    lines = [f"# Renode platform support for the firmware chips{' (' + version + ')' if version else ''}", ""]
    for chip, data in report.items():
        lines.append(f"## {chip}")
        if not data["files"]:
            lines += ["", "NOT SUPPORTED: Renode has no platform description for this family. The firmware of this chip cannot start in Renode until one is written.", ""]
            continue
        lines += ["", "Platform files: " + ", ".join(f"`{name}`" for name in data["files"]), "", "| Peripheral | Declared in |", "|---|---|"]
        for name, where in data["peripherals"].items():
            lines.append(f"| {name} | {', '.join(f'`{w}`' for w in where) if where else 'GAP: not declared'} |")
        lines.append("")
    lines.append("Evidence: a listing of the platform files of the pinned Renode release. No firmware ran.")
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("renode_directory", type=Path)
    parser.add_argument("--markdown", type=Path, help="write the report here as well")
    parser.add_argument("--version", default="")
    args = parser.parse_args(argv)
    if not args.renode_directory.is_dir():
        print(f"not a directory: {args.renode_directory}", file=sys.stderr)
        return 2
    platforms = find_platforms(args.renode_directory)
    if not platforms:
        print("no .repl platform file found: is this an unpacked Renode?", file=sys.stderr)
        return 2
    text = render(analyse(platforms), args.version)
    print(text)
    if args.markdown:
        args.markdown.write_text(text, encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
