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

Nothing here has run on a board. The controller board answers on +0x16 and +0x18 (src/mcu_stm32g474/relay_ids.h), the kinematic brain
waits for those, and the CM5 asks for the next PAGE_ACK or STATUS of a board with POLL_STATUS (+0x17), a frame of the SPI link that the
gateway never puts on the CAN bus. The tests in tests/test_can_ids.py compare the three bootloader_common.h, the firmware sources and
the SPI bridge with this table.
"""
from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path

CONTRACT_PATH = Path(__file__).resolve().parents[1] / "docs" / "can_ids.json"

SLOT_WINDOW = json.loads(CONTRACT_PATH.read_text(encoding="utf-8"))["slot_window"]  # identifiers per slot (mask 0x7C0 in the hardware filter)
SLOT_BASE = json.loads(CONTRACT_PATH.read_text(encoding="utf-8"))["slot_base"]

KINEMATIC_BRAIN = "kinematic-brain"
CONTROLLER_BOARD = "controller-board"
BOOTLOADER = "bootloader"
HOST = "host"


@dataclass(frozen=True)
class CanMessage:
    name: str
    offset: int
    emitter: str


def _load_contract() -> dict:
    """The contract of the SDK (contracts/can_ids.json, `CanIds`), kept here as docs/can_ids.json: the identifiers of the stack bus are written once
    and this module, the firmware tests and the telemetry collector read the same document. tests/test_can_ids.py compares this copy with the SDK when
    the SDK is next to this repository."""
    return json.loads(CONTRACT_PATH.read_text(encoding="utf-8"))


_CONTRACT = _load_contract()
MESSAGES: tuple[CanMessage, ...] = tuple(CanMessage(m["name"], m["offset"], m["emitter"]) for m in _CONTRACT["messages"])

# The emergency stop itself is not inside a slot window: one identifier for the whole stack, below every slot (docs/ESTOP_CAN.md).
ESTOP_CAN_ID = json.loads(CONTRACT_PATH.read_text(encoding="utf-8"))["estop_id"]


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
