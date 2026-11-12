#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/gen_platform_header.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Writes src/mcu_stm32h757/Common/platform_profile.h from docs/platform_profiles.json, so the firmware reads the numbers of a platform from one place.

    python tools/gen_platform_header.py           write the header
    python tools/gen_platform_header.py --check   exit 1 when the header is not what the profiles say

The header holds, for each profile, the SPI role of the link to the host, the owner of each peripheral, the bit timing of the stack bus and the regions of the memory map,
and selects one profile with `HYDRA_PLATFORM` (`HYDRA_PLATFORM_H745` is the default, because that is what the sources build for today; `HYDRA_PLATFORM_H757` selects the
board of production). `HYDRA_SPI_ROLE` may be set on the command line (`HYDRA_SPI_ROLE_MASTER` or `HYDRA_SPI_ROLE_SLAVE`) and otherwise follows the profile: master on the H757,
slave on the H745 (the CM5 path). The stack-bus timing of the CM4 sources is read from it. Evidence: simulator (the header is checked against the profiles)."""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import platform_profile as pp

ROOT = Path(__file__).resolve().parents[1]
HEADER_PATH = ROOT / "src" / "mcu_stm32h757" / "Common" / "platform_profile.h"
OWNER_CODE = {"CM4": 4, "CM7": 7}


def _macro(name: str) -> str:
    return name.upper().replace("-", "_")


def render(document: dict | None = None) -> str:
    document = document or pp.load()
    out: list[str] = []
    w = out.append
    w("/* =============================================================================")
    w(" * HYDRA-UMC - platform_profile.h")
    w(" * GENERATED from docs/platform_profiles.json by tools/gen_platform_header.py - do not edit by hand.")
    w(" * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>")
    w(" * GPL-3.0-or-later - see LICENSE")
    w(" * ============================================================================= */")
    w("#ifndef HYDRA_PLATFORM_PROFILE_H")
    w("#define HYDRA_PLATFORM_PROFILE_H")
    w("")
    w("#define HYDRA_SPI_ROLE_SLAVE 0")
    w("#define HYDRA_SPI_ROLE_MASTER 1")
    w("#define HYDRA_OWNER_CM4 4")
    w("#define HYDRA_OWNER_CM7 7")
    w("")
    w("#if !defined(HYDRA_PLATFORM_H745) && !defined(HYDRA_PLATFORM_H757)")
    w("#define HYDRA_PLATFORM_H745 1 /* what the sources build for today */")
    w("#endif")
    w("#if defined(HYDRA_PLATFORM_H745) && defined(HYDRA_PLATFORM_H757)")
    w("#error \"choose one platform: HYDRA_PLATFORM_H745 or HYDRA_PLATFORM_H757\"")
    w("#endif")
    w("")
    first = True
    for macro, name in (("HYDRA_PLATFORM_H745", "h745_current"), ("HYDRA_PLATFORM_H757", "h757_target")):
        profile = document["profiles"][name]
        w(("#if defined(%s)" if first else "#elif defined(%s)") % macro)
        first = False
        w(f"/* {profile['device']} - {profile['status']} */")
        role = "HYDRA_SPI_ROLE_MASTER" if profile["host"]["spi_role"] == "master" else "HYDRA_SPI_ROLE_SLAVE"
        w("#ifndef HYDRA_SPI_ROLE")
        w(f"#define HYDRA_SPI_ROLE {role} /* default of this platform: the host is {profile['host']['module']} */")
        w("#endif")
        for peripheral, owner in profile["peripheral_owner"].items():
            w(f"#define HYDRA_OWNER_{_macro(peripheral)} HYDRA_OWNER_{owner}")
        bus = profile["stack_bus"]
        w(f"#define HYDRA_STACK_BUS_KERNEL_CLOCK_HZ {bus['kernel_clock_hz']}UL")
        w(f"#define HYDRA_STACK_BUS_PRESCALER {bus['prescaler']}U")
        w(f"#define HYDRA_STACK_BUS_SYNC_JUMP_WIDTH {bus['sync_jump_width']}U")
        w(f"#define HYDRA_STACK_BUS_TIME_SEG1 {bus['time_segment_1']}U")
        w(f"#define HYDRA_STACK_BUS_TIME_SEG2 {bus['time_segment_2']}U")
        w(f"#define HYDRA_STACK_BUS_BIT_RATE_BPS {int(pp.bit_rate(bus))}UL")
        if "pll" in profile:
            w(f"#define HYDRA_CLOCK_HSE_HZ {profile['hse_hz']}UL")
            for pll_name, pll in profile["pll"].items():
                number = pll_name[-1]
                w(f"#define HYDRA_CLOCK_PLL{number}_DIVM {pll['divm']}U")
                w(f"#define HYDRA_CLOCK_PLL{number}_DIVN {pll['divn']}U")
                for output, divider in pp.PLL_DIVIDERS[pll_name].items():
                    w(f"#define HYDRA_CLOCK_{output.upper()}_DIV {divider}U")
            for output, hz in profile["clocks_hz"].items():
                w(f"#define HYDRA_CLOCK_{output.upper()}_HZ {hz}UL")
        for region in profile["memory_map"]["regions"]:
            base = f"HYDRA_MEM_{_macro(region['name'])}"
            w(f"#define {base}_ORIGIN 0x{region['origin']:08X}UL")
            w(f"#define {base}_LENGTH 0x{region['length']:08X}UL")
    w("#endif")
    w("")
    w("#if HYDRA_SPI_ROLE != HYDRA_SPI_ROLE_SLAVE && HYDRA_SPI_ROLE != HYDRA_SPI_ROLE_MASTER")
    w("#error \"HYDRA_SPI_ROLE must be HYDRA_SPI_ROLE_MASTER or HYDRA_SPI_ROLE_SLAVE\"")
    w("#endif")
    w("")
    w("#endif /* HYDRA_PLATFORM_PROFILE_H */")
    return "\n".join(out) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args(argv)
    text = render()
    if args.check:
        current = HEADER_PATH.read_bytes().decode("utf-8").replace("\r\n", "\n") if HEADER_PATH.is_file() else ""
        if current != text:
            print("PLATFORM_HEADER=FAIL platform_profile.h is not what docs/platform_profiles.json says: run tools/gen_platform_header.py", file=sys.stderr)
            return 1
        print("PLATFORM_HEADER=PASS")
        return 0
    HEADER_PATH.write_bytes(text.encode("utf-8"))
    print(f"wrote {HEADER_PATH}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
