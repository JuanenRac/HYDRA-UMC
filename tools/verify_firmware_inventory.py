#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - Verify committed firmware artifact inventory
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0 - see LICENSE
# =============================================================================
"""Fail closed if firmware_manifest.json no longer describes firmware/.

This is intentionally a read-only check.  It verifies the six expected MCU
components, their binary filename/version, byte count and CRC32 before a
release consumer is allowed to trust the committed firmware inventory.
"""

from __future__ import annotations

import json
import re
import sys
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
FIRMWARE = ROOT / "firmware"
MANIFEST = FIRMWARE / "firmware_manifest.json"
EXPECTED_COMPONENTS = {
    "g474_bootloader",
    "g474_application",
    "h745_cm7_bootloader",
    "h745_cm7_application",
    "h745_cm4_bootloader",
    "h745_cm4_application",
}
FILENAME_VERSION = re.compile(r"_v(\d+\.\d+\.\d+)\.bin$")


def fail(message: str) -> None:
    print(f"FIRMWARE_INVENTORY=FAIL {message}", file=sys.stderr)
    raise SystemExit(1)


def main() -> int:
    try:
        payload = json.loads(MANIFEST.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        fail(f"cannot read firmware manifest: {exc}")
    if payload.get("schema_version") != 1:
        fail("unsupported firmware manifest schema")
    components = payload.get("components")
    if not isinstance(components, dict) or set(components) != EXPECTED_COMPONENTS:
        fail("manifest must describe exactly the six expected MCU components")

    for name in sorted(EXPECTED_COMPONENTS):
        component = components[name]
        if not isinstance(component, dict):
            fail(f"{name}: component record is invalid")
        files = component.get("files")
        binary = files.get("bin") if isinstance(files, dict) else None
        if not isinstance(binary, dict):
            fail(f"{name}: missing binary record")
        filename = binary.get("filename")
        if not isinstance(filename, str) or Path(filename).name != filename:
            fail(f"{name}: binary filename is invalid")
        version = component.get("version_string")
        match = FILENAME_VERSION.search(filename)
        if not isinstance(version, str) or not match or match.group(1) != version:
            fail(f"{name}: filename version does not match version_string")
        path = FIRMWARE / filename
        try:
            data = path.read_bytes()
        except OSError as exc:
            fail(f"{name}: cannot read {filename}: {exc}")
        expected_size = binary.get("size_bytes")
        expected_crc = binary.get("crc32")
        actual_crc = f"0x{zlib.crc32(data) & 0xFFFFFFFF:08X}"
        if expected_size != len(data) or expected_crc != actual_crc:
            fail(f"{name}: {filename} does not match recorded size or CRC32")
        for extension in ("hex", "elf"):
            companion = files.get(extension)
            if companion is None:
                continue
            if not isinstance(companion, dict) or not isinstance(companion.get("filename"), str):
                fail(f"{name}: invalid {extension} companion record")
            companion_path = FIRMWARE / companion["filename"]
            if not companion_path.is_file() or companion.get("size_bytes") != companion_path.stat().st_size:
                fail(f"{name}: {extension} companion does not match its record")
        print(f"FIRMWARE_INVENTORY_COMPONENT=PASS name={name} version={version}")
    print("FIRMWARE_INVENTORY=PASS components=6")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
