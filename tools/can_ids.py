#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/can_ids.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Reference table of the CAN identifiers of the relay protocol, one emitter per identifier.

On CAN an identifier must have one emitter: two nodes sending the same identifier with different data or length can collide on the bus
(error frames) and force the receivers to tell them apart by content. The relay used the same identifier for the request of the
kinematic brain and the answer of the controller board, and the host client polled with identifiers that the bootloader itself transmits.
This table gives every message its own identifier inside the window of a slot (`base + offset`), with the emitter that may send it.
The firmware and the clients are to be written against this table; the test in tests/test_can_ids.py keeps it consistent.

Nothing here has run on a board, and the firmware still uses the old identifiers until it is ported.
"""
from __future__ import annotations

from dataclasses import dataclass

SLOT_WINDOW = 0x20  # identifiers per slot (mask 0x7E0 in the hardware filter)

KINEMATIC_BRAIN = "kinematic-brain"
CONTROLLER_BOARD = "controller-board"
BOOTLOADER = "bootloader"
HOST = "host"


@dataclass(frozen=True)
class CanMessage:
    name: str
    offset: int
    emitter: str


MESSAGES: tuple[CanMessage, ...] = (
    # relay protocol
    CanMessage("RELAY_REQUEST", 0x10, KINEMATIC_BRAIN),
    CanMessage("RELAY_REQUEST_EXTRA", 0x13, KINEMATIC_BRAIN),
    CanMessage("RELAY_RESPONSE", 0x16, CONTROLLER_BOARD),  # was the same identifier as the request (0x10)
    CanMessage("RELAY_RESPONSE_EXTRA", 0x18, CONTROLLER_BOARD),  # was the same identifier as the extra request (0x13)
    CanMessage("POLL_STATUS", 0x17, HOST),  # was sent with identifiers the bootloader transmits (0x03 / 0x05)
    # bootloader answers (kept where they are)
    CanMessage("BOOT_PAGE_ACK", 0x03, BOOTLOADER),
    CanMessage("BOOT_STATUS", 0x05, BOOTLOADER),
)


def identifier(slot_base: int, message: CanMessage) -> int:
    if slot_base % SLOT_WINDOW != 0:
        raise ValueError("a slot base must be a multiple of the slot window")
    can_id = slot_base + message.offset
    if can_id > 0x7FF:
        raise ValueError("the identifier does not fit in 11 bits")
    return can_id


def by_name(name: str) -> CanMessage:
    for message in MESSAGES:
        if message.name == name:
            return message
    raise KeyError(name)
