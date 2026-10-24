#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/image_header.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Reference implementation (host side) of the signed firmware image header, its Ed25519 signature and the anti-rollback rule.

Why this exists: the update protocol used to carry the new version in END_UPDATE, outside the data the authentication covers, and compared
only major.minor, so an old signed image could be replayed declaring a higher version, and 0.1.8 -> 0.1.2 was not noticed. Here the version
(major, minor, patch), the hardware id, the link address, the size, the vector words and the CRC of the image are inside a fixed header,
and the signature covers header and image together, so none of them can be changed without breaking it. The rule "a new image must be
strictly newer than the installed one" compares the three numbers.

The signature is Ed25519 (RFC 8032): the board holds only the 32-byte public key, so nothing on it can sign an image. What is signed is
the 52-byte message IMAGE_DOMAIN || SHA-256(header || image), which lets the board hash the image straight out of its flash while it
arrives. A downgrade is authorised by a second signature, over a message that holds the nonce the bootloader chose for the session, the
installed and the target version and the digest of the image, so it is useless in any other session, for any other pair or image.

This module does not touch any firmware: it fixes the byte layout and the decisions with test vectors, so that the bootloader (C) is
checked against bytes the reference made. Nothing here has run on a board.

Header layout (little endian, 64 bytes, then the image):

    offset size field
       0    4   magic            0x52445948 ("HYDR" as bytes 48 59 44 52)
       4    2   format           1
       6    2   flags            0 (must be zero)
       8    4   hw_id            hardware this image is for (the same 32-bit id the bootloader has, e.g. 0x47343031 "G401")
      12    2   major
      14    2   minor
      16    2   patch
      18    2   reserved         0
      20    4   image_size       bytes of image that follow the header
      24    4   link_address     where the image is linked to run
      28    4   vector_sp        initial stack pointer word of the image (equal to its first word)
      32    4   vector_reset     reset vector word of the image (equal to its second word; odd: Thumb)
      36    4   image_crc32      CRC-32 of the image bytes
      40   24   reserved         zeros
    ...  signature  Ed25519 over IMAGE_DOMAIN || SHA-256(header || image) (64 bytes) follows the image

Keys: `keygen` makes a private seed for production; the operator who owns it keeps it off every board and every shared machine. The
development seeds below are published on purpose, only so that a bench build works with no setup; a release build refuses the development
public keys (build_firmware.sh).

    python tools/image_header.py keygen --out SEEDFILE        a new random seed, as 64 hexadecimal digits
    python tools/image_header.py pubkey SEEDFILE              the public key, as hex and as the C initializer the build takes
    python tools/image_header.py sign --seed-file SEEDFILE --hardware-id 0x47343031 --version 0.2.3 --image app.bin [--out app.sig]
"""
from __future__ import annotations

import argparse
import hashlib
import os
import struct
import sys
import zlib
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ed25519_ref as ed25519  # noqa: E402

MAGIC = 0x52445948
FORMAT = 1
HEADER_SIZE = 64
DIGEST_SIZE = 32
SIGNATURE_SIZE = 64
NONCE_SIZE = 16
# The two things that are signed start with a different text, so a signature made for one can never pass for the other.
IMAGE_DOMAIN = b"hydra-umc-image-sig\x00"
DOWNGRADE_DOMAIN = b"hydra-umc-downgrade\x00"
# Published development seeds (one per bootloader), only for bench builds: the matching public keys are what a build without
# HYDRA_FW_UPDATE_PUBKEY_HEX carries, and a release build refuses them. Never use them for anything that is to be trusted.
DEV_SEEDS = {
    "G401": b"HYDRA-UMC-DEV-SEED-G401-0001!!!!",
    "H7C4": b"HYDRA-UMC-DEV-SEED-H7C4-0001!!!!",
    "H7C7": b"HYDRA-UMC-DEV-SEED-H7C7-0001!!!!",
}
assert all(len(seed) == ed25519.SEED_SIZE for seed in DEV_SEEDS.values())
_LAYOUT = struct.Struct("<IHHIHHHHIIIII24s")
assert _LAYOUT.size == HEADER_SIZE


class ImageError(ValueError):
    """The image is not acceptable; the message says why."""


@dataclass(frozen=True)
class ImageHeader:
    hw_id: int
    major: int
    minor: int
    patch: int
    image_size: int
    link_address: int
    vector_sp: int
    vector_reset: int
    image_crc32: int

    @property
    def version(self) -> tuple[int, int, int]:
        return (self.major, self.minor, self.patch)

    def pack(self) -> bytes:
        for name in ("major", "minor", "patch"):
            value = getattr(self, name)
            if not 0 <= value <= 0xFFFF:
                raise ImageError(f"{name} must fit in 16 bits, got {value}")
        for name in ("hw_id", "image_size", "link_address", "vector_sp", "vector_reset", "image_crc32"):
            value = getattr(self, name)
            if not 0 <= value <= 0xFFFFFFFF:
                raise ImageError(f"{name} must fit in 32 bits, got {value}")
        return _LAYOUT.pack(
            MAGIC, FORMAT, 0, self.hw_id, self.major, self.minor, self.patch, 0,
            self.image_size, self.link_address, self.vector_sp, self.vector_reset, self.image_crc32, b"\x00" * 24,
        )


def parse_header(data: bytes) -> ImageHeader:
    if len(data) < HEADER_SIZE:
        raise ImageError("the image is shorter than its header")
    (magic, fmt, flags, hw_id, major, minor, patch, reserved, size, link, sp, reset, crc, padding) = _LAYOUT.unpack(data[:HEADER_SIZE])
    if magic != MAGIC:
        raise ImageError("bad magic: this is not a HYDRA-UMC image")
    if fmt != FORMAT:
        raise ImageError(f"unknown header format {fmt}")
    if flags != 0 or reserved != 0 or padding != b"\x00" * 24:
        raise ImageError("reserved header bytes must be zero")
    return ImageHeader(hw_id, major, minor, patch, size, link, sp, reset, crc)


def image_digest(header_bytes: bytes, image: bytes) -> bytes:
    """SHA-256 over header || image: what the bootloader hashes out of its flash while the image arrives."""
    return hashlib.sha256(header_bytes + image).digest()


def image_message(digest: bytes) -> bytes:
    """The message the image signature is made over."""
    if len(digest) != DIGEST_SIZE:
        raise ImageError(f"the digest of an image is {DIGEST_SIZE} bytes, got {len(digest)}")
    return IMAGE_DOMAIN + digest


def sign_image(seed: bytes, header_bytes: bytes, image: bytes) -> bytes:
    """The 64-byte Ed25519 signature of header || image under the private seed."""
    return ed25519.sign(seed, image_message(image_digest(header_bytes, image)))


def build_signed_image(header: ImageHeader, image: bytes, seed: bytes) -> bytes:
    """header || image || signature, with the size and CRC of the header checked against the image."""
    if header.image_size != len(image):
        raise ImageError("image_size does not match the image")
    if header.image_crc32 != zlib.crc32(image) & 0xFFFFFFFF:
        raise ImageError("image_crc32 does not match the image")
    packed = header.pack()
    return packed + image + sign_image(seed, packed, image)


def downgrade_message(nonce: bytes, installed: tuple[int, int, int], target: tuple[int, int, int], digest: bytes) -> bytes:
    """What a downgrade is authorised by signing: from `installed` to the older `target` with the image whose digest is `digest`, in the
    session the bootloader chose `nonce` for."""
    if len(nonce) != NONCE_SIZE:
        raise ImageError(f"the session nonce is {NONCE_SIZE} bytes, got {len(nonce)}")
    if len(digest) != DIGEST_SIZE:
        raise ImageError(f"the digest of the image is {DIGEST_SIZE} bytes, got {len(digest)}")
    return DOWNGRADE_DOMAIN + nonce + struct.pack("<3H3H", *installed, *target) + digest


def downgrade_token(seed: bytes, nonce: bytes, installed: tuple[int, int, int], target: tuple[int, int, int], digest: bytes) -> bytes:
    """The 64-byte signature that authorises that one downgrade. Not a constant in the firmware, and not a secret on the board."""
    return ed25519.sign(seed, downgrade_message(nonce, installed, target, digest))


def verify_image(
    blob: bytes,
    public_key: bytes,
    *,
    hw_id: int,
    link_address: int,
    installed_version: tuple[int, int, int],
    ram_range: tuple[int, int] | None = None,
    nonce: bytes = b"",
    token: bytes = b"",
) -> ImageHeader:
    """Everything the bootloader decides, in the order it should decide it. Returns the header when the image may be installed."""
    if len(blob) < HEADER_SIZE + SIGNATURE_SIZE:
        raise ImageError("the image is shorter than a header and a signature")
    header = parse_header(blob)
    if len(blob) != HEADER_SIZE + header.image_size + SIGNATURE_SIZE:
        raise ImageError("the size in the header does not match the data received")
    header_bytes = blob[:HEADER_SIZE]
    image = blob[HEADER_SIZE:HEADER_SIZE + header.image_size]
    signature = blob[HEADER_SIZE + header.image_size:]
    digest = image_digest(header_bytes, image)
    # the signature first: nothing in the header is believed before it verifies
    if not ed25519.verify(public_key, image_message(digest), signature):
        raise ImageError("the signature does not verify: the image or its header was changed or signed with another key")
    if zlib.crc32(image) & 0xFFFFFFFF != header.image_crc32:
        raise ImageError("the image does not match the CRC in its header")
    if header.hw_id != hw_id:
        raise ImageError(f"the image is for hardware {header.hw_id}, this board is {hw_id}")
    if header.link_address != link_address:
        raise ImageError("the image is linked for another address")
    if len(image) < 8 or struct.unpack_from("<II", image, 0) != (header.vector_sp, header.vector_reset):
        raise ImageError("the vectors of the header are not the first two words of the image")
    if header.vector_reset & 1 == 0:
        raise ImageError("the reset vector is not a Thumb address")
    reset_address = header.vector_reset & ~1
    if not header.link_address <= reset_address < header.link_address + header.image_size:
        raise ImageError("the reset vector points outside the image")
    if ram_range is not None and not ram_range[0] <= header.vector_sp <= ram_range[1]:
        raise ImageError("the initial stack pointer is outside RAM")
    if header.version <= installed_version:
        # older or equal: only with a signature made for exactly this pair of versions in this session
        if (
            header.version < installed_version
            and len(token) == SIGNATURE_SIZE
            and len(nonce) == NONCE_SIZE
            and ed25519.verify(public_key, downgrade_message(nonce, installed_version, header.version, digest), token)
        ):
            return header
        raise ImageError(
            f"anti-rollback: {'.'.join(map(str, header.version))} is not newer than the installed {'.'.join(map(str, installed_version))}"
        )
    return header


# hardware id -> where that bootloader links its application (MAIN_APP_ADDR of its bootloader_common.h)
LINK_ADDRESS_BY_HARDWARE_ID = {
    0x47343031: 0x08009000,  # "G401": STM32G474, Robot Controller Board
    0x48374334: 0x08140000,  # "H7C4": STM32H745 CM4, Kinematic Brain
    0x48374337: 0x08040000,  # "H7C7": STM32H745 CM7, Kinematic Brain
}


def c_initializer(data: bytes) -> str:
    """The bytes as the comma separated list a C array initializer (or -DHYDRA_FW_UPDATE_PUBKEY_BYTES) takes."""
    return ", ".join(f"0x{byte:02X}" for byte in data)


def _read_seed(path: str) -> bytes:
    text = Path(path).read_text(encoding="ascii").strip()
    try:
        seed = bytes.fromhex(text)
    except ValueError as error:
        raise ImageError(f"{path} must hold the seed as 64 hexadecimal digits") from error
    if len(seed) != ed25519.SEED_SIZE:
        raise ImageError(f"{path} must hold the seed as 64 hexadecimal digits")
    return seed


def _parse_version(text: str) -> tuple[int, int, int]:
    parts = text.split(".")
    if len(parts) != 3 or not all(part.isdigit() for part in parts):
        raise ImageError("the version must be major.minor.patch")
    return (int(parts[0]), int(parts[1]), int(parts[2]))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Keys and signatures of the HYDRA-UMC bootloader images.")
    commands = parser.add_subparsers(dest="command", required=True)
    keygen = commands.add_parser("keygen", help="write a new random private seed (64 hex digits) to a file that did not exist")
    keygen.add_argument("--out", required=True)
    pubkey = commands.add_parser("pubkey", help="print the public key of a seed file")
    pubkey.add_argument("seed_file")
    sign = commands.add_parser("sign", help="sign an application image for a bootloader")
    sign.add_argument("--seed-file", required=True)
    sign.add_argument("--hardware-id", required=True, help="the 32-bit hardware id of the bootloader, e.g. 0x47343031")
    sign.add_argument("--version", required=True, help="major.minor.patch of the image")
    sign.add_argument("--image", required=True)
    sign.add_argument("--link-address", help="where the image runs (default: the one of that hardware id)")
    sign.add_argument("--out", help="write the signature (64 bytes) here instead of printing it as hex")
    args = parser.parse_args(argv)
    try:
        if args.command == "keygen":
            target = Path(args.out)
            seed = os.urandom(ed25519.SEED_SIZE)
            with open(target, "x", encoding="ascii") as handle:  # never overwrites a key
                handle.write(seed.hex() + "\n")
            print(f"seed written to {target}; keep it off the boards and off shared machines")
            print("public key:", ed25519.public_key(seed).hex())
            return 0
        if args.command == "pubkey":
            public = ed25519.public_key(_read_seed(args.seed_file))
            print("hex:", public.hex())
            print("HYDRA_FW_UPDATE_PUBKEY_HEX=" + public.hex())
            print("C:", c_initializer(public))
            return 0
        seed = _read_seed(args.seed_file)
        hardware_id = int(args.hardware_id, 0)
        link = int(args.link_address, 0) if args.link_address else LINK_ADDRESS_BY_HARDWARE_ID.get(hardware_id)
        if link is None:
            raise ImageError("unknown hardware id: give --link-address")
        image = Path(args.image).read_bytes()
        if len(image) < 8:
            raise ImageError("the image is too short to hold its vector table")
        major, minor, patch = _parse_version(args.version)
        sp, reset = struct.unpack_from("<II", image, 0)
        header = ImageHeader(hardware_id, major, minor, patch, len(image), link, sp, reset, zlib.crc32(image) & 0xFFFFFFFF)
        signature = sign_image(seed, header.pack(), image)
        if args.out:
            Path(args.out).write_bytes(signature)
            print(f"signature written to {args.out}")
        else:
            print(signature.hex())
        return 0
    except (ImageError, OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
