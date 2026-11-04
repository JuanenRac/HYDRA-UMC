#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/rdp_plan.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Plans the read-out protection of a production board: decodes the option byte, derives the device identity and prints the programmer command.

    python tools/rdp_plan.py decode 0xAA
    python tools/rdp_plan.py command --level 1 [--programmer STM32_Programmer_CLI]
    python tools/rdp_plan.py device-id --uid 0123456789ABCDEF01234567

Nothing here talks to a board. Level 2 is refused on purpose: it cannot be undone and the bootloaders here do not need it. Level 1 is
reversible, but going back to level 0 erases the whole flash (the bootloader too), so a board in production is re-flashed afterwards.
The switch that decides whether a production flash sets level 1 lives in the machine configuration (`production.rdp_level`, 0 or 1,
default 0); this tool only turns that number into the option byte and the command.
"""
from __future__ import annotations

import argparse
import hashlib
import sys

LEVEL0_BYTE = 0xAA
LEVEL2_BYTE = 0xCC
LEVEL1_DEFAULT_BYTE = 0xBB  # any value that is neither 0xAA nor 0xCC means level 1; 0xBB is the one the programmer tools use
DEVICE_ID_DOMAIN = b"hydra-umc-device-id/1"


def decode(rdp_byte: int) -> int:
    """The protection level an RDP byte stands for (0, 1 or 2)."""
    if not 0 <= rdp_byte <= 0xFF:
        raise ValueError("the RDP option is one byte")
    if rdp_byte == LEVEL0_BYTE:
        return 0
    return 2 if rdp_byte == LEVEL2_BYTE else 1


def option_byte(level: int) -> int:
    """The RDP byte for a level; level 2 is refused because it is permanent."""
    if level == 0:
        return LEVEL0_BYTE
    if level == 1:
        return LEVEL1_DEFAULT_BYTE
    raise ValueError("level 2 is permanent and is not offered: use level 0 or 1")


def command(level: int, programmer: str = "STM32_Programmer_CLI") -> str:
    return f"{programmer} -c port=SWD mode=UR -ob RDP=0x{option_byte(level):02X}"


def device_id(uid: bytes) -> bytes:
    """A 32-byte identity of one chip: SHA-256 over a domain tag and the 96-bit unique ID. It is public information (no secret goes in)."""
    if len(uid) != 12:
        raise ValueError("the unique ID of these chips is 12 bytes")
    return hashlib.sha256(DEVICE_ID_DOMAIN + uid).digest()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="what", required=True)
    one = sub.add_parser("decode")
    one.add_argument("rdp_byte", type=lambda text: int(text, 0))
    two = sub.add_parser("command")
    two.add_argument("--level", type=int, required=True)
    two.add_argument("--programmer", default="STM32_Programmer_CLI")
    three = sub.add_parser("device-id")
    three.add_argument("--uid", required=True, help="the 12-byte unique ID as 24 hex digits")
    args = parser.parse_args(argv)
    try:
        if args.what == "decode":
            print(f"level {decode(args.rdp_byte)}")
        elif args.what == "command":
            print(command(args.level, args.programmer))
        else:
            print(device_id(bytes.fromhex(args.uid)).hex())
    except ValueError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
