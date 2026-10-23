#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - show_component_version.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Print the version one component already has in its bootloader_common.h.

    python3 show_component_version.py <header_path> <MACRO_PREFIX>

MACRO_PREFIX is BOOTLOADER_VERSION or FIRMWARE_VERSION. The file is only read: the build scripts use this to
name the version they are building, and raising a version is the explicit job of `bump_firmware_versions.py`.
A header with a `_BUILD` macro shows a fourth component from 0.8.0 on, like the bump script.
"""
from __future__ import annotations

import re
import sys

FOUR_PART_FROM = (0, 8, 0)


def read_version(text: str, prefix: str) -> str:
    values = []
    for name in ("MAJOR", "MINOR", "PATCH"):
        match = re.search(rf"#define\s+{prefix}_{name}\s+(\d+)", text)
        if match is None:
            raise ValueError(f"#define {prefix}_{name} not found")
        values.append(int(match.group(1)))
    build = re.search(rf"#define\s+{prefix}_BUILD\s+(\d+)", text)
    if build is not None and tuple(values) >= FOUR_PART_FROM:
        values.append(int(build.group(1)))
    return ".".join(str(value) for value in values)


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print("usage: show_component_version.py <header_path> <MACRO_PREFIX>", file=sys.stderr)
        return 1
    try:
        with open(argv[0], encoding="utf-8", errors="replace") as handle:
            print(read_version(handle.read(), argv[1]))
    except (OSError, ValueError) as exc:
        print(f"show_component_version.py: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
