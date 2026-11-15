# =============================================================================
# HYDRA-UMC - tools/nor_backup_plan.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Reference of the backup copy of the CM7 application in the NOR flash (the C twin is src/mcu_stm32h757/Common/nor_backup_plan.h) and the reader of config/nor_backup.cfg: the
layout in the NOR, the 24-byte header and the choice of the newer header. Evidence: simulator."""
from __future__ import annotations

import configparser
import struct
import zlib
from pathlib import Path

CFG_PATH = Path(__file__).resolve().parents[1] / "config" / "nor_backup.cfg"
SECTOR = 4096
BLOCK = 65536
HEADER_BYTES = 24
MAGIC = 0x42524F4E  # "NORB" read as little-endian bytes N O R B


def load_config(path: Path = CFG_PATH) -> dict:
    parser = configparser.ConfigParser(inline_comment_prefixes=None)
    parser.read_string(path.read_text(encoding="utf-8"))
    cfg = {
        "nor_offset": int(parser["nor"]["offset_bytes"]),
        "nor_size": int(parser["nor"]["size_bytes"]),
        "slot_bytes": int(parser["slot"]["app_bytes"]),
    }
    plan(cfg)
    return cfg


def plan(cfg: dict) -> dict:
    """The layout of the copy; raises ValueError for an offset or slot off the erase unit and a copy that does not fit in the NOR."""
    if cfg["slot_bytes"] <= 0 or cfg["nor_size"] <= 0:
        raise ValueError("empty slot or empty NOR")
    if cfg["nor_offset"] % BLOCK or cfg["slot_bytes"] % SECTOR:
        raise ValueError("offset must be a multiple of 64 KB and the slot of 4 KB")
    image_bytes = -(-cfg["slot_bytes"] // BLOCK) * BLOCK
    if cfg["nor_offset"] > cfg["nor_size"] or cfg["nor_size"] - cfg["nor_offset"] < BLOCK + image_bytes:
        raise ValueError("the copy does not fit in the NOR")
    start = cfg["nor_offset"]
    return {"header_a": start, "header_b": start + SECTOR, "image_offset": start + BLOCK, "image_bytes_max": cfg["slot_bytes"], "end": start + BLOCK + image_bytes}


def header_encode(sequence: int, length: int, crc32: int) -> bytes:
    body = struct.pack("<5I", MAGIC, sequence, length, crc32, 0)
    return body + struct.pack("<I", zlib.crc32(body))


def header_decode(raw: bytes, slot_bytes: int) -> dict | None:
    magic, sequence, length, crc32, _zero, check = struct.unpack("<6I", raw)
    if magic != MAGIC or check != zlib.crc32(raw[:20]) or not 0 < length <= slot_bytes:
        return None
    return {"sequence": sequence, "length": length, "crc32": crc32}


def newest(a: dict | None, b: dict | None) -> tuple[dict, int] | None:
    """The newer header and 0 (A) or 1 (B); the sequence compares by wrapping arithmetic, as the C code does."""
    if a and b:
        ahead = (b["sequence"] - a["sequence"]) & 0xFFFFFFFF
        return (b, 1) if 0 < ahead < 0x80000000 else (a, 0)
    if a:
        return a, 0
    if b:
        return b, 1
    return None
