# =============================================================================
# HYDRA-UMC - tools/boot_nor_stage.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Reference of how the bootloader of the CM7 stages an update in the NOR flash and recovers from it (the C twin is src/mcu_stm32h757/Common/boot_nor_stage.h) and the reader of
config/nor_spi5.cfg (the C twin of the pins is src/mcu_stm32h757/Common/nor_spi5_pins.h). The model works on two bytearrays (the NOR and the application slot) with the same layout, header and
rules as tools/nor_backup_plan.py: begin, page, finish (header last, only if the CRC-32 matches), commit (only when the copy is the image the caller expects) and the boot-time rule.
Evidence: simulator."""
from __future__ import annotations

import configparser
import zlib
from pathlib import Path

import nor_backup_plan as nb

PINS_PATH = Path(__file__).resolve().parents[1] / "config" / "nor_spi5.cfg"
PORT_COUNT = 11  # GPIOA to GPIOK
TIMEOUT_MAX_MS = 1000
CHUNK = 256

RECOVER_NONE, RECOVER_DONE, RECOVER_LOST = 0, 1, 2


def parse_pin(text: str) -> tuple[int, int]:
    """'F6' is port 5, pin 6."""
    text = text.strip().upper()
    if len(text) < 2 or not text[0].isalpha() or not text[1:].isdigit():
        raise ValueError(f"not a pin: {text!r}")
    port, pin = ord(text[0]) - ord("A"), int(text[1:])
    if port >= PORT_COUNT or pin > 15:
        raise ValueError(f"pin outside the chip: {text}")
    return port, pin


def load_pins(path: Path = PINS_PATH) -> dict:
    """The pins and the timeout of config/nor_spi5.cfg, checked as nor_spi5_pins.h checks them."""
    parser = configparser.ConfigParser(inline_comment_prefixes=None)
    parser.read_string(path.read_text(encoding="utf-8"))
    cs_port, cs_pin = parse_pin(parser["pins"]["cs"])
    sck_port, sck = parse_pin(parser["pins"]["sck"])
    miso_port, miso = parse_pin(parser["pins"]["miso"])
    mosi_port, mosi = parse_pin(parser["pins"]["mosi"])
    pins = {
        "cs_port": cs_port,
        "cs_pin": cs_pin,
        "bus_port": sck_port,
        "sck_pin": sck,
        "miso_pin": miso,
        "mosi_pin": mosi,
        "alternate": int(parser["pins"]["alternate"]),
        "timeout_ms": int(parser["spi"]["transfer_timeout_ms"]),
    }
    if not (sck_port == miso_port == mosi_port):
        raise ValueError("the three pins of the bus must be on the same port")
    problems = pin_problems(pins)
    if problems:
        raise ValueError("; ".join(problems))
    return pins


def bus_mask(pins: dict) -> int:
    return (1 << pins["sck_pin"]) | (1 << pins["miso_pin"]) | (1 << pins["mosi_pin"])


def pin_problems(pins: dict) -> list[str]:
    """Every reason a set of pins is refused (the checks of NorS5_Valid)."""
    out = []
    if pins["cs_port"] >= PORT_COUNT or pins["bus_port"] >= PORT_COUNT:
        out.append("port outside the chip")
    if max(pins["cs_pin"], pins["sck_pin"], pins["miso_pin"], pins["mosi_pin"], pins["alternate"]) > 15:
        out.append("pin or alternate function above 15")
    if len({pins["sck_pin"], pins["miso_pin"], pins["mosi_pin"]}) != 3:
        out.append("two pins of the bus are the same")
    elif pins["cs_port"] == pins["bus_port"] and bus_mask(pins) & (1 << pins["cs_pin"]):
        out.append("the chip select is a pin of the bus")
    if not 1 <= pins["timeout_ms"] <= TIMEOUT_MAX_MS:
        out.append("timeout out of range")
    return out


class Stage:
    """The staging area: a NOR and an application slot as bytearrays, with the plan of nor_backup_plan.py."""

    def __init__(self, nor: bytearray, flash: bytearray, cfg: dict):
        self.nor, self.flash, self.cfg = nor, flash, cfg
        self.layout = nb.plan(cfg)

    def begin(self, total: int) -> None:
        if not 0 < total <= self.cfg["slot_bytes"]:
            raise ValueError("too big")
        start = self.layout["image_offset"]
        end = start + -(-total // nb.SECTOR) * nb.SECTOR
        self.nor[start:end] = b"\xff" * (end - start)

    def page(self, index: int, page_bytes: int, data: bytes) -> None:
        offset = index * page_bytes
        if not data or offset > self.cfg["slot_bytes"] or len(data) > self.cfg["slot_bytes"] - offset:
            raise ValueError("too big")
        where = self.layout["image_offset"] + offset
        for i, byte in enumerate(data):  # a NOR can only clear bits
            self.nor[where + i] &= byte

    def headers(self) -> tuple[dict, int] | None:
        raw = []
        for at in (self.layout["header_a"], self.layout["header_b"]):
            raw.append(nb.header_decode(bytes(self.nor[at : at + nb.HEADER_BYTES]), self.cfg["slot_bytes"]))
        return nb.newest(raw[0], raw[1])

    def finish(self, total: int, crc32: int) -> None:
        if not 0 < total <= self.cfg["slot_bytes"]:
            raise ValueError("too big")
        start = self.layout["image_offset"]
        if zlib.crc32(bytes(self.nor[start : start + total])) != crc32:
            raise ValueError("copy bad")
        found = self.headers()
        sequence = found[0]["sequence"] + 1 if found else 1
        into_b = bool(found) and found[1] == 0
        at = self.layout["header_b"] if into_b else self.layout["header_a"]
        self.nor[at : at + nb.SECTOR] = b"\xff" * nb.SECTOR
        raw = nb.header_encode(sequence & 0xFFFFFFFF, total, crc32)
        self.nor[at : at + len(raw)] = raw

    def copy_is(self, length: int, crc32: int) -> bool:
        found = self.headers()
        return bool(found) and found[0]["length"] == length and found[0]["crc32"] == crc32

    def copy_exists(self) -> bool:
        found = self.headers()
        if not found:
            return False
        start = self.layout["image_offset"]
        return zlib.crc32(bytes(self.nor[start : start + found[0]["length"]])) == found[0]["crc32"]

    def commit(self, length: int, crc32: int) -> None:
        if not self.copy_is(length, crc32):
            raise ValueError("mismatch")
        if not self.copy_exists():
            raise ValueError("copy bad")
        start = self.layout["image_offset"]
        self.flash[:] = b"\xff" * len(self.flash)
        self.flash[:length] = self.nor[start : start + length]

    def recover(self, application_good: bool, length: int, crc32: int) -> int:
        """The boot-time rule: nothing when the application is good, a restore when it is not and the copy is the image of the metadata, lost otherwise."""
        if application_good:
            return RECOVER_NONE
        if not (self.copy_is(length, crc32) and self.copy_exists()):
            return RECOVER_LOST
        self.commit(length, crc32)
        return RECOVER_DONE
