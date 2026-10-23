#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - bump_firmware_versions.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Raise the version of firmware components, explicitly, before a build.

    python3 bump_firmware_versions.py                  # all six components, then the manifest
    python3 bump_firmware_versions.py g474-app cm7-app # only the named components
    python3 bump_firmware_versions.py --list           # the names, the headers and the current versions

Building never changes a version; this is the one place where the six component versions (three bootloaders and
three applications) are raised, one step each, with the odometer rule of `bump_version.py`. The project manifest
follows the version of the G474 application (`bump_manifest_version.py --sync`) when that component is bumped.
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
G4 = "src/mcu_stm32g474/boot/bootloader_common.h"
CM7 = "src/mcu_stm32h745/CM7/boot/bootloader_common.h"
CM4 = "src/mcu_stm32h745/CM4/boot/bootloader_common.h"
# name -> (header, macro prefix, label)
COMPONENTS = {
    "g474-boot": (G4, "BOOTLOADER_VERSION", "Robot Controller Board bootloader"),
    "g474-app": (G4, "FIRMWARE_VERSION", "Robot Controller Board application"),
    "cm7-boot": (CM7, "BOOTLOADER_VERSION", "Kinematic Brain CM7 bootloader"),
    "cm7-app": (CM7, "FIRMWARE_VERSION", "Kinematic Brain CM7 application"),
    "cm4-boot": (CM4, "BOOTLOADER_VERSION", "Kinematic Brain CM4 bootloader"),
    "cm4-app": (CM4, "FIRMWARE_VERSION", "Kinematic Brain CM4 application"),
}


def current(name: str, root: Path = ROOT) -> str:
    header, prefix, _ = COMPONENTS[name]
    result = subprocess.run(
        [sys.executable, str(root / "show_component_version.py"), str(root / header), prefix],
        capture_output=True, text=True, check=False,
    )
    return result.stdout.strip() if result.returncode == 0 else "?"


def bump(names: list[str], root: Path = ROOT) -> int:
    for name in names:
        header, prefix, label = COMPONENTS[name]
        result = subprocess.run(
            [sys.executable, str(root / "bump_version.py"), str(root / header), prefix],
            capture_output=True, text=True, check=False,
        )
        if result.returncode != 0:
            print(f"BUMP=FAIL {label}: {result.stderr.strip()}", file=sys.stderr)
            return 1
        print(f"{label}: {result.stdout.strip()}")
    if "g474-app" in names:
        sync = subprocess.run([sys.executable, str(root / "bump_manifest_version.py"), "--sync"], cwd=root, check=False)
        if sync.returncode != 0:
            print("BUMP=FAIL the project manifest could not follow the G474 application", file=sys.stderr)
            return 1
    print("BUMP=PASS")
    return 0


def main(argv: list[str]) -> int:
    if argv == ["--list"]:
        for name, (header, prefix, label) in COMPONENTS.items():
            print(f"{name:10s} {current(name):10s} {label} ({header} {prefix})")
        return 0
    unknown = [name for name in argv if name not in COMPONENTS]
    if unknown:
        print("unknown component(s): " + ", ".join(unknown) + "; known: " + ", ".join(COMPONENTS), file=sys.stderr)
        return 2
    return bump(argv or list(COMPONENTS))


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
