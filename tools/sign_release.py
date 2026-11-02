#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/sign_release.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Signs the application images a build made and checks the signatures it finds.

    python tools/sign_release.py sign   [--dir firmware] [--seed-file FILE] [--require]
    python tools/sign_release.py verify [--dir firmware] [--public-key-hex HEX] [--require]

For every HYDRA_<board>_APP_v<major>.<minor>.<patch>.bin of the folder, `sign` writes HYDRA_<board>_APP_v<...>.sig next to it: the Ed25519
signature over the signed image header and the image (tools/image_header.py). The private seed is only ever read from a file that lives
outside this repository: --seed-file, or the file named by HYDRA_FW_SIGNING_SEED_FILE. Without a seed nothing is signed and the command says
so (exit 0); with --require, or HYDRA_UMC_RELEASE=1, a missing seed is an error. The seed is never printed.

`verify` checks every .sig against the public key (--public-key-hex or HYDRA_FW_UPDATE_PUBKEY_HEX, the same variable the bootloaders are
built with). A .bin without a .sig is reported; with --require it is an error. Signatures only ever cover application images: the
bootloaders are flashed over the debug port, not updated through the bootloader.
"""
from __future__ import annotations

import argparse
import os
import re
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ed25519_ref as ed25519  # noqa: E402
import image_header as ih  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent

HARDWARE_ID_BY_BOARD = {
    "RCB": 0x47343031,
    "KB_CM4": 0x48374334,
    "KB_CM7": 0x48374337,
}
_APP = re.compile(r"^HYDRA_(?P<board>RCB|KB_CM4|KB_CM7)_APP_v(?P<major>\d+)\.(?P<minor>\d+)\.(?P<patch>\d+)\.bin$")


def application_images(folder: Path) -> list[tuple[Path, int, tuple[int, int, int]]]:
    """(path, hardware id, version) of each application image in the folder, in name order."""
    found = []
    for path in sorted(folder.glob("HYDRA_*_APP_v*.bin")):
        match = _APP.match(path.name)
        if match:
            version = (int(match["major"]), int(match["minor"]), int(match["patch"]))
            found.append((path, HARDWARE_ID_BY_BOARD[match["board"]], version))
    return found


def header_for(image: bytes, hardware_id: int, version: tuple[int, int, int]) -> ih.ImageHeader:
    if len(image) < 8:
        raise ih.ImageError("the image is too short to hold its vector table")
    sp, reset = struct.unpack_from("<II", image, 0)
    link = ih.LINK_ADDRESS_BY_HARDWARE_ID[hardware_id]
    return ih.ImageHeader(hardware_id, *version, len(image), link, sp, reset, zlib.crc32(image) & 0xFFFFFFFF)


def sign_folder(folder: Path, seed: bytes) -> list[Path]:
    written = []
    for path, hardware_id, version in application_images(folder):
        image = path.read_bytes()
        header = header_for(image, hardware_id, version)
        signature = ih.sign_image(seed, header.pack(), image)
        target = path.with_suffix(".sig")
        target.write_bytes(signature)
        written.append(target)
    return written


def verify_folder(folder: Path, public_key: bytes) -> tuple[list[Path], list[Path], list[Path]]:
    """(verified, bad, unsigned) signature files / images."""
    good: list[Path] = []
    bad: list[Path] = []
    unsigned: list[Path] = []
    for path, hardware_id, version in application_images(folder):
        sig = path.with_suffix(".sig")
        if not sig.is_file():
            unsigned.append(path)
            continue
        image = path.read_bytes()
        header = header_for(image, hardware_id, version)
        digest = ih.image_digest(header.pack(), image)
        ok = ed25519.verify(public_key, ih.image_message(digest), sig.read_bytes())
        (good if ok else bad).append(sig)
    return good, bad, unsigned


def _parse_public(text: str) -> bytes:
    try:
        key = bytes.fromhex(text.strip())
    except ValueError as error:
        raise ih.ImageError("the public key must be 64 hexadecimal digits") from error
    if len(key) != 32:
        raise ih.ImageError("the public key must be 64 hexadecimal digits")
    return key


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Sign the application images of a build and verify their signatures.")
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("sign", "verify"):
        sub = commands.add_parser(name)
        sub.add_argument("--dir", default=str(ROOT / "firmware"))
        sub.add_argument("--require", action="store_true", help="a missing seed / key / signature is an error")
        if name == "sign":
            sub.add_argument("--seed-file", default=os.environ.get("HYDRA_FW_SIGNING_SEED_FILE"))
        else:
            sub.add_argument("--public-key-hex", default=os.environ.get("HYDRA_FW_UPDATE_PUBKEY_HEX"))
    args = parser.parse_args(argv)
    required = args.require or os.environ.get("HYDRA_UMC_RELEASE") == "1"
    folder = Path(args.dir)
    try:
        if args.command == "sign":
            if not args.seed_file:
                print("SIGN_RELEASE=SKIPPED no signing seed (--seed-file / HYDRA_FW_SIGNING_SEED_FILE): the images stay unsigned")
                return 1 if required else 0
            written = sign_folder(folder, ih._read_seed(args.seed_file))
            for target in written:
                print(f"signed {target.name}")
            if not written:
                print("SIGN_RELEASE=FAIL no application image found in " + str(folder))
                return 1
            print(f"SIGN_RELEASE=PASS signatures={len(written)}")
            return 0
        if not args.public_key_hex:
            print("VERIFY_RELEASE=SKIPPED no public key (--public-key-hex / HYDRA_FW_UPDATE_PUBKEY_HEX)")
            return 1 if required else 0
        good, bad, unsigned = verify_folder(folder, _parse_public(args.public_key_hex))
        for path in bad:
            print(f"BAD signature {path.name}", file=sys.stderr)
        for path in unsigned:
            print(f"unsigned {path.name}")
        failed = bool(bad) or (required and bool(unsigned))
        print(f"VERIFY_RELEASE={'FAIL' if failed else 'PASS'} verified={len(good)} bad={len(bad)} unsigned={len(unsigned)}")
        return 1 if failed else 0
    except (ih.ImageError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
