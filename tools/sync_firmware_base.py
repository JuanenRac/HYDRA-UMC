#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/sync_firmware_base.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The shared part of the three STM32 bootloaders (G474, H745 CM4, H745 CM7): one copy in firmware_base/boot, the same bytes in each chip directory.

    python tools/sync_firmware_base.py status   what is shared, what differs, and by how many lines
    python tools/sync_firmware_base.py check    fails when a copy in a chip directory is not the base byte for byte (CI runs it)
    python tools/sync_firmware_base.py sync     writes the base over the copies of the chip directories (the base is edited, never a copy)
    python tools/sync_firmware_base.py import   takes the files that are already identical in all three chip directories into the base and the list

Why files and not an include path: every chip directory has to build on its own (build_firmware.sh, the host tests and the Renode and Actions jobs name
those directories), so a copy stays where the compiler looks for it and this tool keeps the copies from drifting. Nothing here changes a line of any
bootloader: `import` only declares what was already the same. What differs between the chips (bootloader_common.h, the flash routines, the protocol)
stays in the chip directories and is listed by `status` with the number of lines that differ, so the next step of the split can be sized.
Standard library only. Evidence of anything built on this file: source check."""
from __future__ import annotations

import argparse
import difflib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BASE = ROOT / "firmware_base" / "boot"
LIST = ROOT / "firmware_base" / "shared.json"
CHIPS = {
    "g474": ROOT / "src" / "mcu_stm32g474" / "boot",
    "h745_cm4": ROOT / "src" / "mcu_stm32h745" / "CM4" / "boot",
    "h745_cm7": ROOT / "src" / "mcu_stm32h745" / "CM7" / "boot",
}
SOURCE_SUFFIXES = (".c", ".h")


def read_list() -> list[str]:
    if not LIST.exists():
        return []
    return list(json.loads(LIST.read_text(encoding="utf-8"))["shared"])


def write_list(names: list[str]) -> None:
    LIST.parent.mkdir(parents=True, exist_ok=True)
    LIST.write_text(json.dumps({"shared": sorted(names)}, indent=2) + "\n", encoding="utf-8", newline="\n")


def chip_sources(chip_dir: Path) -> dict[str, bytes]:
    return {path.name: path.read_bytes() for path in chip_dir.iterdir() if path.is_file() and path.suffix in SOURCE_SUFFIXES}


def line_difference(left: bytes, right: bytes) -> int:
    """The number of lines that differ between two files, ignoring line endings."""
    a = left.decode("utf-8", "replace").replace("\r\n", "\n").splitlines()
    b = right.decode("utf-8", "replace").replace("\r\n", "\n").splitlines()
    return sum(1 for line in difflib.unified_diff(a, b, lineterm="", n=0) if line[:1] in "+-" and line[:3] not in ("+++", "---"))


def problems() -> list[str]:
    """Every way the copies drifted from the base: a file missing from the base or from a chip, or a chip copy that is not the base."""
    found = []
    for name in read_list():
        base_file = BASE / name
        if not base_file.exists():
            found.append(f"{name}: listed as shared but missing from firmware_base/boot")
            continue
        base = base_file.read_bytes()
        for chip, directory in CHIPS.items():
            copy = directory / name
            if not copy.exists():
                found.append(f"{name}: missing from {chip}")
            elif copy.read_bytes() != base:
                found.append(f"{name}: the copy in {chip} is not the base ({line_difference(base, copy.read_bytes())} lines differ)")
    listed = set(read_list())
    for path in sorted(BASE.glob("*")) if BASE.exists() else []:
        if path.name not in listed:
            found.append(f"{path.name}: in firmware_base/boot but not in shared.json")
    return found


def identical_everywhere() -> list[str]:
    per_chip = [chip_sources(directory) for directory in CHIPS.values()]
    names = set(per_chip[0])
    for sources in per_chip[1:]:
        names &= set(sources)
    return sorted(name for name in names if len({sources[name] for sources in per_chip}) == 1)


def cmd_status() -> int:
    shared = read_list()
    print(f"shared: {len(shared)} files in firmware_base/boot")
    for name in shared:
        print(f"  {name}")
    per_chip = {chip: chip_sources(directory) for chip, directory in CHIPS.items()}
    own = sorted({name for sources in per_chip.values() for name in sources} - set(shared))
    print(f"of each chip: {len(own)} files")
    for name in own:
        present = {chip: sources[name] for chip, sources in per_chip.items() if name in sources}
        if len(present) == 1:
            print(f"  {name}: only {next(iter(present))}")
            continue
        chips = list(present)
        reference = present[chips[0]]
        spread = ", ".join(f"{chip} {line_difference(reference, present[chip])}" for chip in chips[1:])
        print(f"  {name}: {'identical' if all(v == reference for v in present.values()) else 'differs'} (lines against {chips[0]}: {spread})")
    return 0


def cmd_check() -> int:
    found = problems()
    for line in found:
        print(f"FIRMWARE_BASE: {line}")
    print("FIRMWARE_BASE=" + ("PASS" if not found else f"FAIL ({len(found)})"))
    return 1 if found else 0


def cmd_sync() -> int:
    for name in read_list():
        base = (BASE / name).read_bytes()
        for directory in CHIPS.values():
            target = directory / name
            if not target.exists() or target.read_bytes() != base:
                target.write_bytes(base)
                print(f"wrote {target}")
    return cmd_check()


def cmd_import() -> int:
    BASE.mkdir(parents=True, exist_ok=True)
    names = sorted(set(read_list()) | set(identical_everywhere()))
    for name in names:
        source = next(directory / name for directory in CHIPS.values() if (directory / name).exists())
        (BASE / name).write_bytes(source.read_bytes())
    write_list(names)
    print(f"{len(names)} files are shared")
    return cmd_check()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Keep the shared bootloader files of the three STM32 chips identical.")
    parser.add_argument("command", choices=("status", "check", "sync", "import"))
    options = parser.parse_args(argv)
    return {"status": cmd_status, "check": cmd_check, "sync": cmd_sync, "import": cmd_import}[options.command]()


if __name__ == "__main__":
    sys.exit(main())
