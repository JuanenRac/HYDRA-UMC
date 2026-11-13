#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/tmc_card.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Reference of the datagrams of the TMC5160 drivers on the expansion cards and of the choice of the card on SPI6 (the C twin is src/mcu_stm32h757/Common/tmc_card.h).

The select lines EXP_GPIO1 (bit 0) and EXP_GPIO2 (bit 1) carry the number of the slot (1 to 3, 0 is none) and the general chip select PG3 is the strobe. A datagram is 5 bytes:
the register address (bit 7 set for a write) and 32 bits of data; the answer carries the status byte, and the data of a read comes with the NEXT datagram. Evidence: simulator."""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Callable

WRITE_BIT = 0x80
SLOTS = 3
STATUS_RESET_FLAG, STATUS_DRIVER_ERROR, STATUS_STALLGUARD, STATUS_STANDSTILL = 1, 2, 4, 8

OK, ERR_SLOT, ERR_NOT_READY, ERR_TRANSFER = 0, 1, 2, 3


def encode_write(reg: int, value: int) -> bytes:
    return bytes([WRITE_BIT | (reg & 0x7F)]) + (value & 0xFFFFFFFF).to_bytes(4, "big")


def encode_read(reg: int) -> bytes:
    return bytes([reg & 0x7F]) + bytes(4)


def decode_reply(raw: bytes) -> tuple[int, int]:
    return raw[0], int.from_bytes(raw[1:5], "big")


@dataclass
class Card:
    transfer: Callable[[bytes], bytes | None]
    log: list = field(default_factory=list)  # ("select", bits), ("cs", level), ("transfer", bytes) in order
    select: int = 0
    cs: int = 1

    def exchange(self, slot: int, ready_mask: int, tx: bytes) -> tuple[int, tuple[int, int] | None]:
        if not 1 <= slot <= SLOTS:
            return ERR_SLOT, None
        if not ready_mask >> (slot - 1) & 1:
            return ERR_NOT_READY, None
        self._cs(1)
        self._select(slot)
        self._cs(0)
        raw = self.transfer(tx)
        self.log.append(("transfer", tx))
        self._cs(1)
        self._select(0)
        if raw is None:
            return ERR_TRANSFER, None
        return OK, decode_reply(raw)

    def _cs(self, level: int) -> None:
        self.cs = level
        self.log.append(("cs", level))

    def _select(self, bits: int) -> None:
        assert self.cs == 1, "the select lines may only change while the strobe is high"
        self.select = bits
        self.log.append(("select", bits))

    def write(self, slot: int, ready_mask: int, reg: int, value: int):
        return self.exchange(slot, ready_mask, encode_write(reg, value))

    def read(self, slot: int, ready_mask: int, reg: int):
        status, _ = self.exchange(slot, ready_mask, encode_read(reg))
        if status != OK:
            return status, None
        return self.exchange(slot, ready_mask, encode_read(reg))
