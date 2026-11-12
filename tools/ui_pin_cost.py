#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/ui_pin_cost.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The estimated cost of checking the operator PIN on the Cortex-M7, and what it buys against guessing.

    python tools/ui_pin_cost.py          print the estimate and exit 1 when a check is slower than the limit of config/ui.cfg

The number of SHA-256 compressions of one check is EXACT: the C store counts them in the host test (`UI_PIN_COUNT_BLOCKS`) and the formula here, 2 x iterations + 2, is held equal to
that count. What is an ESTIMATE is the cycles per compression (`pin_cycles_per_block`, about 40 cycles per byte of a software SHA-256, and `pin_hw_cycles_per_block` for the hash
peripheral of the H757) and the clock of the core (`sysclk` of the h757_target profile); none was measured on a board. The guessing figures use the lock-out of the view-model and
count only the wait of the lock-out, which is the part a person cannot skip, not the check itself. Evidence: simulator (arithmetic); nothing here ran on a board."""
from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ui_model  # noqa: E402
import ui_pin  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


def compressions(iterations: int) -> int:
    """SHA-256 compressions of one PBKDF2-HMAC-SHA256 block of 32 bytes: two for the padded keys, two for the first message, two for each of the other iterations."""
    return 2 * iterations + 2


def sysclk_hz() -> int:
    profile = json.loads((ROOT / "docs" / "platform_profiles.json").read_text(encoding="utf-8"))["profiles"]["h757_target"]
    return int(profile["clocks_hz"]["sysclk"])


def estimate() -> dict:
    settings = ui_pin.load_settings()
    cfg = ui_model.load_config()
    blocks = compressions(settings["pin_hash_iterations"])
    clock = sysclk_hz()
    software_ms = blocks * settings["pin_cycles_per_block"] / clock * 1000
    hardware_ms = blocks * settings["pin_hw_cycles_per_block"] / clock * 1000
    tries_per_cycle = cfg["pin_max_failures"]
    seconds_per_cycle = cfg["pin_lockout_ms"] / 1000
    space = 10 ** settings["pin_min_digits"]
    # a guess costs one try of the lock-out cycle; the check itself is in the margin
    hours_to_exhaust = space / tries_per_cycle * seconds_per_cycle / 3600
    return {
        "iterations": settings["pin_hash_iterations"],
        "compressions": blocks,
        "clock_hz": clock,
        "software_ms": software_ms,
        "hardware_ms": hardware_ms,
        "limit_ms": settings["pin_check_max_ms"],
        "fits": software_ms <= settings["pin_check_max_ms"],
        "pin_space": space,
        "tries_per_lockout": tries_per_cycle,
        "lockout_s": seconds_per_cycle,
        "hours_to_try_every_pin": hours_to_exhaust,
    }


def calibrate(measured_cycles: int, iterations: int) -> dict:
    """What a measurement on the board (the cycles of ui_pin_bench for `iterations`) says: the cycles per compression to put in config/ui.cfg and the time of a check at the clock of the profile."""
    blocks = compressions(iterations)
    clock = sysclk_hz()
    return {"compressions": blocks, "cycles_per_block": measured_cycles / blocks, "milliseconds": measured_cycles / clock * 1000}


def report(numbers: dict) -> list[str]:
    return [
        f"one check: {numbers['iterations']} iterations = {numbers['compressions']} SHA-256 compressions (exact)",
        f"on a core at {numbers['clock_hz'] / 1e6:.0f} MHz: about {numbers['software_ms']:.0f} ms in software, about {numbers['hardware_ms']:.0f} ms with the hash peripheral "
        f"(estimates, limit {numbers['limit_ms']} ms: {'fits' if numbers['fits'] else 'DOES NOT FIT'})",
        f"guessing: {numbers['tries_per_lockout']} tries per lock-out of {numbers['lockout_s']:.0f} s; every PIN of the shortest length ({numbers['pin_space']} of them) "
        f"takes about {numbers['hours_to_try_every_pin']:.0f} hours of lock-outs, and a restart does not reset the count",
    ]


def main() -> int:
    numbers = estimate()
    for line in report(numbers):
        print(line)
    print("ui_pin_cost:", "PASS" if numbers["fits"] else "FAIL")
    return 0 if numbers["fits"] else 1


if __name__ == "__main__":
    sys.exit(main())
