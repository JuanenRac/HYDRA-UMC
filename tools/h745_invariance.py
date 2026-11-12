#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/h745_invariance.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Shows that a change to the firmware leaves the images of the default platform exactly as they were.

The firmware of the default platform is what runs on the bench boards today; a change that is made for the H757 platform only (behind the platform profile) must not alter a byte
of it. The proof is a comparison of the six images that build_firmware.sh makes, built from two commits by the same toolchain in the same job layout:

    python tools/h745_invariance.py collect firmware > sums.txt        one line per image: the sha256 and the component (the version in the file name is dropped)
    python tools/h745_invariance.py compare base.txt head.txt           exit 1 when an image differs, is missing or is new

The workflow .github/workflows/h745-invariance.yml builds both commits and runs the comparison; it is started by hand. A change that is MEANT to alter the images (a fix in the
firmware of the default platform) fails the comparison on purpose, and the difference lists which components changed. Evidence: simulator (the comparison is run by tests on
sums files); the real proof is the workflow run on the two commits."""
from __future__ import annotations

import argparse
import hashlib
import re
import sys
from pathlib import Path

COMPONENTS = ("KB_CM4_APP", "KB_CM4_BOOTLOADER", "KB_CM7_APP", "KB_CM7_BOOTLOADER", "RCB_APP", "RCB_BOOTLOADER")
IMAGE_NAME = re.compile(r"HYDRA_(?P<component>[A-Z0-9_]+?)_v\d+\.\d+\.\d+(?:\.\d+)?\.bin")


def collect(directory: Path) -> dict[str, str]:
    """The sha256 of each image of the directory, by component. Raises when an image is missing or twice there."""
    sums: dict[str, str] = {}
    for image in sorted(directory.glob("HYDRA_*.bin")):
        match = IMAGE_NAME.fullmatch(image.name)
        if not match:
            continue
        component = match.group("component")
        if component in sums:
            raise ValueError(f"two images for {component}")
        sums[component] = hashlib.sha256(image.read_bytes()).hexdigest()
    missing = [c for c in COMPONENTS if c not in sums]
    if missing:
        raise ValueError("missing images: " + ", ".join(missing))
    return sums


def render(sums: dict[str, str]) -> str:
    return "".join(f"{sums[c]}  {c}\n" for c in sorted(sums))


def parse(text: str) -> dict[str, str]:
    sums: dict[str, str] = {}
    for line in text.splitlines():
        if not line.strip():
            continue
        match = re.fullmatch(r"([0-9a-f]{64})  ([A-Z0-9_]+)", line.strip())
        if not match:
            raise ValueError(f"not a line of sums: {line!r}")
        sums[match.group(2)] = match.group(1)
    return sums


def compare(base: dict[str, str], head: dict[str, str]) -> list[str]:
    """What differs between two sets of images: a component whose bytes changed, one that only one side has."""
    found: list[str] = []
    for component in sorted(set(base) | set(head)):
        if component not in head:
            found.append(f"{component}: built from the base commit only")
        elif component not in base:
            found.append(f"{component}: built from the head commit only")
        elif base[component] != head[component]:
            found.append(f"{component}: the image changed ({base[component][:12]} -> {head[component][:12]})")
    return found


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    sub = parser.add_subparsers(dest="command", required=True)
    one = sub.add_parser("collect")
    one.add_argument("directory", type=Path)
    two = sub.add_parser("compare")
    two.add_argument("base", type=Path)
    two.add_argument("head", type=Path)
    args = parser.parse_args(argv)
    if args.command == "collect":
        try:
            sys.stdout.write(render(collect(args.directory)))
        except ValueError as error:
            print(f"H745_INVARIANCE=FAIL {error}", file=sys.stderr)
            return 1
        return 0
    try:
        found = compare(parse(args.base.read_text(encoding="utf-8")), parse(args.head.read_text(encoding="utf-8")))
    except ValueError as error:
        print(f"H745_INVARIANCE=FAIL {error}", file=sys.stderr)
        return 1
    for line in found:
        print("DIFFERENT " + line, file=sys.stderr)
    print("H745_INVARIANCE=" + ("FAIL" if found else "PASS"))
    return 1 if found else 0


if __name__ == "__main__":
    raise SystemExit(main())
