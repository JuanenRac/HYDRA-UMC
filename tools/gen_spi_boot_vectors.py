#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/gen_spi_boot_vectors.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Writes tests/spi_boot_master_vectors.json and tests/spi_boot_master_vectors.h: the frames of the SPI boot protocol as the Python model builds them.

    python tools/gen_spi_boot_vectors.py           write both files
    python tools/gen_spi_boot_vectors.py --check   exit 1 when a file is not what the model produces

The C header src/mcu_stm32h757/Common/spi_boot_master.h is tested against these vectors by tests/test_spi_boot_master.c, so the firmware and the model cannot drift
apart. Evidence: simulator."""
from __future__ import annotations

import argparse
import json
import struct
import zlib
import sys
from pathlib import Path

import spi_boot_master as sbm

ROOT = Path(__file__).resolve().parents[1]
JSON_PATH = ROOT / "tests" / "spi_boot_master_vectors.json"
HEADER_PATH = ROOT / "tests" / "spi_boot_master_vectors.h"
NONCE = bytes(range(16))
STATUS = {"shorter": 1, "CRC": 2, "sync": 3, "length": 4, "type": 5}


def status_of(frame: bytes, host: bool) -> int:
    try:
        sbm.decode(frame, sbm.HOST_TYPES if host else sbm.MASTER_TYPES)
    except sbm.FrameError as error:
        for word, code in STATUS.items():
            if word in str(error):
                return code
        raise
    return 0


def build() -> dict:
    cfg = sbm.BootConfig(spi_master_enabled=True)
    hello = sbm.hello_payload(cfg, NONCE)
    frames = [
        ("hello", sbm.T_HELLO, 1, hello, False),
        ("get_page", sbm.T_GET_PAGE, 2, struct.pack("<II", 512, 256), False),
        ("get_token", sbm.T_GET_TOKEN, 3, b"", False),
        ("done", sbm.T_DONE, 65535, b"", False),
        ("offer", sbm.T_OFFER, 1, sbm.offer_payload(1428, (0, 1, 8), (0, 1, 8), "H757-CM7"), True),
        ("offer_no_min_no_target", sbm.T_OFFER, 1, sbm.offer_payload(1428, (0, 1, 8), None, None), True),
        ("page", sbm.T_PAGE, 2, struct.pack("<I", 256) + bytes(range(8)), True),
        ("page_full", sbm.T_PAGE, 3, struct.pack("<I", 0) + bytes(range(256)), True),
        ("token", sbm.T_TOKEN, 4, bytes(range(64)), True),
        ("refuse", sbm.T_REFUSE, 5, b"done", True),
    ]
    good = [
        {"name": n, "type": t, "sequence": s, "payload_hex": p.hex(), "frame_hex": sbm.encode(t, s, p).hex(), "host": h}
        for n, t, s, p, h in frames
    ]
    page = sbm.encode(sbm.T_PAGE, 7, b"abcdef")
    bad_cases = [
        ("short", page[:5], True),
        ("one_byte_short", page[:-1], True),
        ("trailing_byte", page + b"\x00", True),
        ("crc_flipped", page[:-1] + bytes([page[-1] ^ 1]), True),
        ("payload_flipped", page[:8] + bytes([page[8] ^ 0x10]) + page[9:], True),
        ("hello_where_host_expected", sbm.encode(sbm.T_HELLO, 1, b""), True),
        ("page_where_master_expected", page, False),
    ]
    # a frame with a sync byte that is not 0x53 but a right CRC, and one whose length field disagrees but whose CRC is right
    body = struct.pack("<BBHH", 0x54, sbm.T_PAGE, 1, 0)
    bad_sync = body + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)
    body = struct.pack("<BBHH", sbm.SYNC, sbm.T_PAGE, 1, 9) + b"x" * 3
    bad_length = body + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)
    bad_cases += [("sync_not_0x53", bad_sync, True), ("length_field_disagrees", bad_length, True)]
    bad = [{"name": n, "frame_hex": f.hex(), "host": h, "status": status_of(f, h)} for n, f, h in bad_cases]
    judge_cfg = {"max_image_bytes": sbm.MAX_IMAGE_BYTES, "board_target": cfg.target, "bootloader": list(cfg.bootloader_version)}
    offers = []
    for name, size, version, minimum, target in (
        ("good", 1428, (0, 1, 8), (0, 1, 8), "H757-CM7"),
        ("no_min_no_target", 1428, (0, 1, 8), None, None),
        ("size_too_small", 127, (0, 1, 8), None, None),
        ("size_smallest", 128, (0, 1, 8), None, None),
        ("size_largest", sbm.MAX_IMAGE_BYTES, (0, 1, 8), None, None),
        ("size_too_big", sbm.MAX_IMAGE_BYTES + 1, (0, 1, 8), None, None),
        ("wrong_target", 1428, (0, 1, 8), None, "G474-CTRL"),
        ("target_is_a_prefix", 1428, (0, 1, 8), None, "H757"),
        ("target_longer", 1428, (0, 1, 8), None, "H757-CM7X"),
        ("min_bootloader_newer_patch", 1428, (0, 1, 8), (0, 1, 9), None),
        ("min_bootloader_newer_minor", 1428, (0, 1, 8), (0, 2, 0), None),
        ("min_bootloader_newer_major", 1428, (0, 1, 8), (1, 0, 0), None),
        ("min_bootloader_equal", 1428, (0, 1, 8), (0, 1, 8), None),
        ("min_bootloader_older", 1428, (0, 1, 8), (0, 0, 9), None),
    ):
        payload = sbm.offer_payload(size, version, minimum, target)
        offers.append({"name": name, "payload_hex": payload.hex(), "size": size, "target": target or "", "verdict": sbm.judge_offer(size, target or "", minimum, cfg)})
    hello_args = {"hw_id": cfg.hw_id, "link_address": cfg.link_address, "installed": list(cfg.installed_version), "nonce_hex": NONCE.hex(), "bootloader": list(cfg.bootloader_version), "sequence": 1}
    return {"schema": 1, "frames": good, "bad_frames": bad, "judge": judge_cfg, "offers": offers, "hello": hello_args}


def render_header(vectors: dict) -> str:
    out = ["/* GENERATED by tools/gen_spi_boot_vectors.py from tests/spi_boot_master_vectors.json - do not edit by hand. */", "#ifndef HYDRA_UMC_SPI_BOOT_MASTER_VECTORS_H", "#define HYDRA_UMC_SPI_BOOT_MASTER_VECTORS_H", ""]
    out.append("typedef struct { const char *name; unsigned type; unsigned sequence; const char *payload_hex; const char *frame_hex; int host; } SbmFrameVector;")
    out.append("typedef struct { const char *name; const char *frame_hex; int host; int status; } SbmBadFrameVector;")
    out.append("typedef struct { const char *name; const char *payload_hex; unsigned size; const char *target; int verdict; } SbmOfferVector;")
    out.append("")
    out.append("static const SbmFrameVector SBM_FRAMES[] = {")
    for f in vectors["frames"]:
        out.append(f'    {{"{f["name"]}", {f["type"]}u, {f["sequence"]}u, "{f["payload_hex"]}", "{f["frame_hex"]}", {int(f["host"])}}},')
    out.append("};")
    out.append("static const SbmBadFrameVector SBM_BAD_FRAMES[] = {")
    for f in vectors["bad_frames"]:
        out.append(f'    {{"{f["name"]}", "{f["frame_hex"]}", {int(f["host"])}, {f["status"]}}},')
    out.append("};")
    out.append("static const SbmOfferVector SBM_OFFERS[] = {")
    for o in vectors["offers"]:
        out.append(f'    {{"{o["name"]}", "{o["payload_hex"]}", {o["size"]}u, "{o["target"]}", {o["verdict"]}}},')
    out.append("};")
    j = vectors["judge"]
    out.append(f'#define SBM_JUDGE_MAX_IMAGE_BYTES {j["max_image_bytes"]}u')
    out.append(f'#define SBM_JUDGE_BOARD_TARGET "{j["board_target"]}"')
    out.append("static const uint16_t SBM_JUDGE_BOOTLOADER[3] = {" + ", ".join(f"{v}u" for v in j["bootloader"]) + "};")
    h = vectors["hello"]
    out.append(f'#define SBM_HELLO_HW_ID {h["hw_id"]}u')
    out.append(f'#define SBM_HELLO_LINK {h["link_address"]}u')
    out.append(f'#define SBM_HELLO_SEQUENCE {h["sequence"]}u')
    out.append("static const uint16_t SBM_HELLO_INSTALLED[3] = {" + ", ".join(f"{v}u" for v in h["installed"]) + "};")
    out.append("static const uint16_t SBM_HELLO_BOOTLOADER[3] = {" + ", ".join(f"{v}u" for v in h["bootloader"]) + "};")
    out.append("")
    out.append("#endif")
    return "\n".join(out) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args(argv)
    vectors = build()
    wanted = {JSON_PATH: json.dumps(vectors, indent=2) + "\n", HEADER_PATH: render_header(vectors)}
    if args.check:
        stale = [p.name for p, text in wanted.items() if not p.is_file() or p.read_bytes().decode("utf-8").replace("\r\n", "\n") != text]
        if stale:
            print("SPI_BOOT_VECTORS=FAIL " + ", ".join(stale) + " is not what the model produces: run tools/gen_spi_boot_vectors.py", file=sys.stderr)
            return 1
        print("SPI_BOOT_VECTORS=PASS")
        return 0
    for path, text in wanted.items():
        path.write_bytes(text.encode("utf-8"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
