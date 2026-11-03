#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/gen_image_format2_vectors.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Writes tests/image_format2_vectors.json (and the same cases as tests/image_format2_vectors.h for the C host tests): header format 2 images, their signatures and the decision a bootloader with a trust table takes, as the host
reference (tools/image_header2.py, tools/image_trust.py) makes them. Every key is a throwaway test key derived from a label: no secret is here.

    python tools/gen_image_format2_vectors.py          writes the file
    python tools/gen_image_format2_vectors.py --check  exits 1 when the file is not what the reference makes
"""
from __future__ import annotations

import hashlib
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ed25519_ref as ed25519  # noqa: E402
import image_header as ih  # noqa: E402
import image_header2 as h2  # noqa: E402
import image_trust as tr  # noqa: E402

OUTPUT = Path(__file__).resolve().parent.parent / "tests" / "image_format2_vectors.json"
OUTPUT_C = OUTPUT.with_suffix(".h")
HW_ID = 0x47343031
LINK = 0x08009000
INSTALLED = (0, 2, 2)
NONCE = bytes(range(16))


def seed(label: str) -> bytes:
    return hashlib.sha256(b"hydra-umc throwaway test key format2 " + label.encode()).digest()


def public(label: str) -> bytes:
    return ed25519.public_key(seed(label))


def image(salt: int, size: int = 96) -> bytes:
    return struct.pack("<II", 0x20010000, LINK + 0x41) + bytes((i * salt + 3) & 0xFF for i in range(size - 8))


def flip(data: bytes, index: int) -> bytes:
    out = bytearray(data)
    out[index] ^= 1
    return bytes(out)


def build(label: str = "current", version: tuple[int, int, int] = (0, 2, 3), counter: int = 5, salt: int = 7, hw: int = HW_ID, link: int = LINK) -> bytes:
    body = image(salt)
    header = h2.make_header2(seed(label), body, hw, version, link, counter)
    return header.pack() + body + h2.sign_image2(seed(label), header, body)


def cases() -> list[dict]:
    cur, nxt = public("current"), public("next")
    good = build()
    body = image(7)
    header = good[:64]
    digest = hashlib.sha256(header + body).digest()
    older = build(version=(0, 1, 8), salt=11)
    older_digest = hashlib.sha256(older[:64] + older[64:-64]).digest()
    token = ih.downgrade_token(seed("current"), NONCE, INSTALLED, (0, 1, 8), older_digest)
    format1 = bytearray(good)
    format1[4] = 1
    spec: list[tuple[str, dict]] = [
        ("a good image signed by the current key", {}),
        ("signed by the next key when both are in the table", {"blob": build("next"), "trusted": [cur, nxt]}),
        ("signed by the next key when the table lists only the current one", {"blob": build("next")}),
        ("signed by a key nobody lists", {"blob": build("stranger")}),
        ("signed by a key whose entry is revoked (not trusted)", {"blob": build("old"), "trusted": [cur]}),
        ("a counter equal to the floor", {"floor": 5}),
        ("a counter below the floor", {"floor": 6}),
        ("a flipped bit in the image", {"blob": flip(good, 64 + 40)}),
        ("a flipped bit in the signature", {"blob": flip(good, len(good) - 3)}),
        ("a flipped bit in the counter of the header", {"blob": flip(good, 40)}),
        ("a flipped bit in the key id of the header", {"blob": flip(good, 44)}),
        ("a flipped bit in the version of the header", {"blob": flip(good, 16)}),
        ("a header of format 1 is not format 2", {"blob": bytes(format1)}),
        ("another hardware id", {"hw_id": 0x48374337}),
        ("another link address", {"link_address": 0x08040000}),
        ("the same version is not newer", {"installed_version": (0, 2, 3)}),
        ("an older version without a token", {"blob": older}),
        ("an older version with the token of this session", {"blob": older, "nonce": NONCE, "token": token}),
        ("an older version with a token of another session", {"blob": older, "nonce": bytes(16), "token": token}),
        ("an older version with a token but a counter below the floor", {"blob": older, "nonce": NONCE, "token": token, "floor": 6}),
        ("too short", {"blob": good[:100]}),
        ("a byte more than the header says", {"blob": good + b"\x00"}),
        ("a signature of format 1 does not pass for format 2", {"blob": good[:-64] + ed25519.sign(seed("current"), ih.image_message(digest))}),
    ]
    out = []
    for name, over in spec:
        args = {"blob": good, "trusted": [cur], "hw_id": HW_ID, "link_address": LINK, "floor": 0, "installed_version": INSTALLED, "nonce": b"", "token": b""}
        args.update(over)
        expected = h2.decide2(args["blob"], args["trusted"], hw_id=args["hw_id"], link_address=args["link_address"], floor=args["floor"],
                              installed_version=args["installed_version"], nonce=args["nonce"], token=args["token"])
        out.append({"name": name, "blob": args["blob"].hex(), "trusted": [k.hex() for k in args["trusted"]], "hw_id": args["hw_id"],
                    "link_address": args["link_address"], "floor": args["floor"], "installed_version": list(args["installed_version"]),
                    "nonce": args["nonce"].hex(), "token": args["token"].hex(), "expected": expected})
    return out


def table_cases() -> list[dict]:
    cur, nxt, old = public("current"), public("next"), public("old")
    good = tr.pack_table([(cur, 1), (nxt, 2)])
    one = tr.pack_table([(cur, 1)])
    revoked = tr.pack_table([(old, 4), (cur, 1)])
    return [
        {"name": "one current key", "table": one.hex(), "valid": True, "trusted": [cur.hex()]},
        {"name": "current and next", "table": good.hex(), "valid": True, "trusted": [cur.hex(), nxt.hex()]},
        {"name": "a revoked key stays and is not trusted", "table": revoked.hex(), "valid": True, "trusted": [cur.hex()]},
        {"name": "bad magic", "table": flip(good, 0).hex(), "valid": False, "trusted": []},
        {"name": "a flipped bit in a key", "table": flip(good, 20).hex(), "valid": False, "trusted": []},
        {"name": "a flipped bit in the CRC", "table": flip(good, len(good) - 1).hex(), "valid": False, "trusted": []},
        {"name": "cut short", "table": good[:-1].hex(), "valid": False, "trusted": []},
    ]


def render() -> str:
    header = h2.make_header2(seed("current"), image(7), HW_ID, (0, 2, 3), LINK, 5).pack()
    doc = {
        "format": 1,
        "about": "Header format 2 and trust table vectors. Keys are throwaway test keys derived from labels (tools/gen_image_format2_vectors.py); no secret is here.",
        "domain_2": h2.IMAGE_DOMAIN_2.hex(),
        "header_example": header.hex(),
        "table_cases": table_cases(),
        "decision_cases": cases(),
    }
    return json.dumps(doc, indent=1) + "\n"


def render_c() -> str:
    """The same decision and table cases as a C header (tests/image_format2_vectors.h) for the host tests of the bootloaders. The verdicts are the
    names of the host reference, numbered by their place in image_header2.STATUS."""
    def array(data: bytes) -> str:
        return ", ".join(f"0x{b:02X}" for b in data) if data else "0"

    lines = [
        "// Generated by tools/gen_image_format2_vectors.py - do not edit. Throwaway test keys only; no secret is here.",
        "#ifndef IMAGE_FORMAT2_VECTORS_H",
        "#define IMAGE_FORMAT2_VECTORS_H",
        "#include <stdint.h>",
        "",
    ]
    for number, name in enumerate(h2.STATUS):
        lines.append(f"#define F2_STATUS_{name.upper()} {number}")
    lines.append("")
    decisions = cases()
    for index, case in enumerate(decisions):
        blob = bytes.fromhex(case["blob"]) if isinstance(case["blob"], str) else case["blob"]
        lines.append(f"static const uint8_t f2_blob_{index}[] = {{{array(blob)}}};")
    lines += [
        "",
        "typedef struct {",
        "    const char *name;",
        "    const uint8_t *blob;",
        "    uint32_t blob_len;",
        "    uint8_t trusted_count;",
        "    uint8_t trusted[2][32];",
        "    uint32_t hw_id, link_address, floor;",
        "    uint16_t installed[3];",
        "    uint8_t nonce_len, nonce[16];",
        "    uint8_t token_len, token[64];",
        "    int expected;",
        "} Format2Case;",
        "",
        "static const Format2Case F2_CASES[] = {",
    ]
    for index, case in enumerate(decisions):
        blob = case["blob"]
        trusted = [bytes.fromhex(k) for k in case["trusted"]] + [bytes(32)] * (2 - len(case["trusted"]))
        nonce, token = bytes.fromhex(case["nonce"]), bytes.fromhex(case["token"])
        lines.append(
            f'    {{"{case["name"]}", f2_blob_{index}, {len(bytes.fromhex(blob))}u, {len(case["trusted"])}, '
            f"{{{{{array(trusted[0])}}}, {{{array(trusted[1])}}}}}, "
            f'0x{case["hw_id"]:08X}u, 0x{case["link_address"]:08X}u, {case["floor"]}u, '
            f'{{{case["installed_version"][0]}, {case["installed_version"][1]}, {case["installed_version"][2]}}}, '
            f"{len(nonce)}, {{{array(nonce)}}}, {len(token)}, {{{array(token)}}}, F2_STATUS_{case['expected'].upper()}}},"
        )
    lines += ["};", f"#define F2_CASE_COUNT {len(decisions)}u", ""]
    tables = table_cases()
    for index, case in enumerate(tables):
        lines.append(f"static const uint8_t f2_table_{index}[] = {{{array(bytes.fromhex(case['table']))}}};")
    lines += [
        "",
        "typedef struct {",
        "    const char *name;",
        "    const uint8_t *table;",
        "    uint32_t table_len;",
        "    int valid;",
        "    uint8_t trusted_count;",
        "    uint8_t trusted[2][32];",
        "} Format2TableCase;",
        "",
        "static const Format2TableCase F2_TABLE_CASES[] = {",
    ]
    for index, case in enumerate(tables):
        keys = [bytes.fromhex(k) for k in case["trusted"]] + [bytes(32)] * (2 - len(case["trusted"]))
        lines.append(
            f'    {{"{case["name"]}", f2_table_{index}, {len(bytes.fromhex(case["table"]))}u, {1 if case["valid"] else 0}, {len(case["trusted"])}, '
            f"{{{{{array(keys[0])}}}, {{{array(keys[1])}}}}}}},"
        )
    lines += ["};", f"#define F2_TABLE_CASE_COUNT {len(tables)}u", "", "#endif", ""]
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    text = render()
    text_c = render_c()
    if argv and argv[0] == "--check":
        stale = []
        for path, wanted in ((OUTPUT, text), (OUTPUT_C, text_c)):
            current = path.read_bytes().decode("utf-8").replace("\r\n", "\n") if path.is_file() else ""
            if current != wanted:
                stale.append(path.name)
        if stale:
            print(f"{', '.join(stale)} out of date: run tools/gen_image_format2_vectors.py", file=sys.stderr)
            return 1
        print(f"{OUTPUT.name} and {OUTPUT_C.name} are current")
        return 0
    OUTPUT.write_text(text, encoding="utf-8", newline="\n")
    OUTPUT_C.write_text(text_c, encoding="utf-8", newline="\n")
    print(f"wrote {OUTPUT} and {OUTPUT_C}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
