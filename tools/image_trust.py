#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/image_trust.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The trust table of the bootloaders: its bytes, its public source file (keys/image_trust.json) and the check that a compiled bootloader holds exactly it.

    python tools/image_trust.py validate                      the source file is well formed
    python tools/image_trust.py table FLAVOUR [--c]           the table bytes (hexadecimal, or a C array for the linker section)
    python tools/image_trust.py check BIN TARGET FLAVOUR      BIN holds, at the fixed offset of TARGET, exactly the table of FLAVOUR
    python tools/image_trust.py fingerprints FLAVOUR          the key ids the local release tool pins
    python tools/image_trust.py entry SEEDFILE [--state current|next]   the public half of a seed file as the JSON entry to paste into the source file

Layout (little endian): ``TKEY`` | schema 1 | number of keys (1 or 2) | flags 0 (u16) | per key: key id (8) | public key (32) | flags (1: current, 2: next,
4: revoked) | 3 zero bytes | CRC-32 (zlib) of all of it. 56 bytes with one key, 100 with two.

Provisioning the image-signing key of the applications (procedure; this repository never makes or keeps the key):
  1. On the maintainer's machine, outside every repository:  ``python tools/image_header.py keygen --out ~/.hydra-umc-keys/hydra_image_ed25519.seed``
     (refuses to overwrite a file; keep an offline copy; the key that signs release assets is a different one).
  2. ``python tools/image_trust.py entry ~/.hydra-umc-keys/hydra_image_ed25519.seed --state current`` prints ONLY the public entry; paste it in the release
     flavour of keys/image_trust.json, make a second seed the same way for ``next``, and run ``validate``.
  3. Only the public file is committed. The seed is never printed, copied into a repository, an artifact or a build service.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ed25519_ref as ed25519  # noqa: E402
import image_header as ih  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
TRUST_FILE = ROOT / "keys" / "image_trust.json"
STATES = {"current": 1, "next": 2, "revoked": 4}
KEY_ENTRY_SIZE = 44
KNOWN_FLAGS = 7


class TrustError(ValueError):
    pass


def key_id(public: bytes) -> bytes:
    return hashlib.sha512(public).digest()[:8]


def table_size(count: int) -> int:
    return 8 + KEY_ENTRY_SIZE * count + 4


def _flags_ok(flags: int) -> bool:
    return flags != 0 and not flags & ~KNOWN_FLAGS and not (flags & 4 and flags != 4) and not (flags & 1 and flags & 2)


def pack_table(keys: list[tuple[bytes, int]]) -> bytes:
    if not 1 <= len(keys) <= 2:
        raise TrustError("a trust table holds one or two keys")
    body = b"TKEY" + struct.pack("<BBH", 1, len(keys), 0)
    for public, flags in keys:
        if len(public) != 32 or not _flags_ok(flags):
            raise TrustError("a key is 32 bytes and its flags a valid state")
        body += key_id(public) + public + bytes([flags]) + bytes(3)
    return body + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)


def parse_table(data: bytes) -> list[tuple[bytes, int]]:
    """Decode and validate a table (bytes after it are ignored). Every doubt is an error."""
    if len(data) < table_size(1) or data[:4] != b"TKEY":
        raise TrustError("no trust table here (bad magic)")
    schema, count, table_flags = struct.unpack_from("<BBH", data, 4)
    if schema != 1 or count not in (1, 2) or table_flags != 0:
        raise TrustError("unknown schema, number of keys or table flags")
    size = table_size(count)
    if len(data) < size or struct.unpack_from("<I", data, size - 4)[0] != zlib.crc32(data[:size - 4]) & 0xFFFFFFFF:
        raise TrustError("the table is cut short or its CRC-32 does not match")
    keys = []
    for index in range(count):
        base = 8 + index * KEY_ENTRY_SIZE
        public, flags = data[base + 8:base + 40], data[base + 40]
        if data[base:base + 8] != key_id(public) or data[base + 41:base + 44] != bytes(3) or not _flags_ok(flags):
            raise TrustError("a key entry is not valid (key id, flags or padding)")
        keys.append((public, flags))
    if len({k[0] for k in keys}) != count or sum(1 for _, f in keys if f & 1) > 1:
        raise TrustError("a key is listed twice or there is more than one current key")
    return keys


def trusted_keys(table: list[tuple[bytes, int]]) -> list[bytes]:
    """Every key that is not revoked, the current one first."""
    return [p for p, f in sorted((e for e in table if not e[1] & 4), key=lambda e: 0 if e[1] & 1 else 1)]


def load(path: Path = TRUST_FILE) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def validate(doc: dict) -> None:
    if doc.get("format") != 1 or doc.get("schema") != 1:
        raise TrustError("unknown format or schema")
    if doc.get("default_policy") != "format2_only":
        raise TrustError("the default policy must be format2_only (fails closed)")
    if not isinstance(doc.get("min_counter"), int) or not 0 <= doc["min_counter"] <= 0xFFFFFFFF:
        raise TrustError("min_counter is a 32-bit number")
    for name, target in doc.get("targets", {}).items():
        if not str(target.get("table_offset", "")).startswith("0x") or int(target["table_offset"], 16) % 4:
            raise TrustError(f"target {name}: table_offset must be a word-aligned hexadecimal number")
    if set(doc.get("flavours", {})) != {"dev", "release"}:
        raise TrustError("the flavours are exactly dev and release")
    for flavour, body in doc["flavours"].items():
        keys = body["keys"]
        if len(keys) > 2 or sum(1 for e in keys if e["state"] == "current") > 1 or len({e["public"] for e in keys}) != len(keys):
            raise TrustError(f"{flavour}: at most two different keys and one current")
        for entry in keys:
            public = bytes.fromhex(entry["public"])
            if len(public) != 32 or entry["state"] not in STATES or entry["key_id"] != key_id(public).hex():
                raise TrustError(f"{flavour}: a key is 32 bytes, in a legal state, and its key_id is the hash prefix of the key")
        if flavour == "dev" and not any(e["state"] == "current" for e in keys):
            raise TrustError("the dev flavour needs a current key")


def table_bytes(doc: dict, flavour: str) -> bytes:
    validate(doc)
    keys = doc["flavours"][flavour]["keys"]
    if not any(e["state"] == "current" for e in keys):
        raise TrustError(f"the {flavour} table has no current key: it cannot be built (the image-signing key is not provisioned yet)")
    return pack_table([(bytes.fromhex(e["public"]), STATES[e["state"]]) for e in keys])


def check_binary(doc: dict, binary: bytes, target: str, flavour: str) -> str | None:
    expected = table_bytes(doc, flavour)
    offset = int(doc["targets"][target]["table_offset"], 16)
    if binary[offset:offset + len(expected)] == expected:
        return None
    if binary[offset:offset + 4] != b"TKEY":
        return f"no trust table at offset 0x{offset:X}"
    return f"the table at offset 0x{offset:X} differs from the {flavour} table of the public file"


def fingerprints(doc: dict, flavour: str) -> list[str]:
    validate(doc)
    return [e["key_id"] for e in doc["flavours"][flavour]["keys"]]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("validate")
    table = commands.add_parser("table")
    table.add_argument("flavour", choices=("dev", "release"))
    table.add_argument("--c", action="store_true")
    check = commands.add_parser("check")
    check.add_argument("binary")
    check.add_argument("target")
    check.add_argument("flavour", choices=("dev", "release"))
    pins = commands.add_parser("fingerprints")
    pins.add_argument("flavour", choices=("dev", "release"))
    entry = commands.add_parser("entry")
    entry.add_argument("seed_file")
    entry.add_argument("--state", choices=("current", "next"), default="current")
    args = parser.parse_args(argv)
    try:
        if args.command == "entry":
            public = ed25519.public_key(ih._read_seed(args.seed_file))   # the seed is read, never printed
            print(json.dumps({"state": args.state, "public": public.hex(), "key_id": key_id(public).hex()}))
            return 0
        doc = load()
        if args.command == "validate":
            validate(doc)
            print("IMAGE_TRUST=PASS")
        elif args.command == "table":
            data = table_bytes(doc, args.flavour)
            print(", ".join(f"0x{b:02X}" for b in data) if args.c else data.hex())
        elif args.command == "fingerprints":
            print("\n".join(fingerprints(doc, args.flavour)))
        else:
            if args.target not in doc["targets"]:
                raise TrustError(f"unknown target {args.target}: {', '.join(doc['targets'])}")
            problem = check_binary(doc, Path(args.binary).read_bytes(), args.target, args.flavour)
            if problem:
                print("IMAGE_TRUST=FAIL " + problem, file=sys.stderr)
                return 1
            print("IMAGE_TRUST=PASS the bootloader holds the " + args.flavour + " table")
        return 0
    except (TrustError, ih.ImageError, OSError, KeyError, ValueError) as error:
        print(f"IMAGE_TRUST=FAIL {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
