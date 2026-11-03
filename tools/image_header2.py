#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/image_header2.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Reference (host side) of the signed image header, format 2: the rollback counter and the key id inside the signed bytes, and the decision a bootloader
with a trust table takes. Format 1 (tools/image_header.py) is untouched and still what the bootloaders in the field read.

Nothing here runs on a board and no bootloader reads format 2 yet; it fixes the bytes with shared vectors (tools/gen_image_format2_vectors.py).

Header (little endian, 64 bytes): the first 40 bytes of format 1 with ``format`` = 2, then

    offset size field
       40    4  rollback_counter   an image is refused when it is below the floor the bootloader keeps (and the floor compiled into it)
       44    8  key_id             the first 8 bytes of SHA-512 of the public key that signs
       52   12  reserved           zeros

The signature is Ed25519 over ``IMAGE_DOMAIN_2 || SHA-256(header || image)`` with a domain that differs from format 1's, so a signature of one format is never
good for the other. The trust table (``TKEY``) is the same bytes as the URTC one; see tools/image_trust.py. The authorised downgrade of format 1 is kept.
"""
from __future__ import annotations

import hashlib
import struct
import sys
import zlib
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ed25519_ref as ed25519  # noqa: E402
import image_header as ih  # noqa: E402

FORMAT = 2
HEADER_SIZE = ih.HEADER_SIZE
IMAGE_DOMAIN_2 = b"hydra-umc-image-sig/2\x00"
# The seed of the key a bench build trusts. Public on purpose: a bootloader with the dev table is a bench build, never an official one.
DEV_SEED = hashlib.sha256(b"hydra-umc public development image key v1").digest()
_LAYOUT = struct.Struct("<IHHIHHHHIIIIII8s12s")
assert _LAYOUT.size == HEADER_SIZE

STATUS = ("ok", "too_short", "unknown_format", "bad_header", "size_mismatch", "unknown_key", "bad_signature", "bad_crc", "hardware_mismatch",
          "link_mismatch", "bad_vectors", "rollback_counter", "rollback_version")


def key_id(public: bytes) -> bytes:
    return hashlib.sha512(public).digest()[:8]


@dataclass(frozen=True)
class Header2:
    hw_id: int
    major: int
    minor: int
    patch: int
    image_size: int
    link_address: int
    vector_sp: int
    vector_reset: int
    image_crc32: int
    rollback_counter: int
    key_id: bytes

    @property
    def version(self) -> tuple[int, int, int]:
        return (self.major, self.minor, self.patch)

    def pack(self) -> bytes:
        for name in ("major", "minor", "patch"):
            if not 0 <= getattr(self, name) <= 0xFFFF:
                raise ih.ImageError(f"{name} must fit in 16 bits")
        for name in ("hw_id", "image_size", "link_address", "vector_sp", "vector_reset", "image_crc32", "rollback_counter"):
            if not 0 <= getattr(self, name) <= 0xFFFFFFFF:
                raise ih.ImageError(f"{name} must fit in 32 bits")
        if len(self.key_id) != 8:
            raise ih.ImageError("a key id is 8 bytes")
        return _LAYOUT.pack(ih.MAGIC, FORMAT, 0, self.hw_id, self.major, self.minor, self.patch, 0, self.image_size, self.link_address,
                            self.vector_sp, self.vector_reset, self.image_crc32, self.rollback_counter, self.key_id, bytes(12))


def parse_header2(data: bytes) -> Header2:
    if len(data) < HEADER_SIZE:
        raise ih.ImageError("the image is shorter than its header")
    (magic, fmt, flags, hw, major, minor, patch, reserved, size, link, sp, reset, crc, counter, kid, padding) = _LAYOUT.unpack(data[:HEADER_SIZE])
    if magic != ih.MAGIC:
        raise ih.ImageError("bad magic: this is not a HYDRA-UMC image")
    if fmt != FORMAT:
        raise ih.ImageError(f"unknown header format {fmt}")
    if flags != 0 or reserved != 0 or padding != bytes(12):
        raise ih.ImageError("reserved header bytes must be zero")
    return Header2(hw, major, minor, patch, size, link, sp, reset, crc, counter, kid)


def image_message2(header_bytes: bytes, image: bytes) -> bytes:
    return IMAGE_DOMAIN_2 + hashlib.sha256(header_bytes + image).digest()


def make_header2(seed: bytes, image: bytes, hw_id: int, version: tuple[int, int, int], link: int, counter: int) -> Header2:
    """The header for ``image``, naming the key of ``seed`` as the signer. The vector words are the first two words of the image."""
    if len(image) < 8:
        raise ih.ImageError("the image is too short to hold its vector table")
    sp, reset = struct.unpack_from("<II", image, 0)
    return Header2(hw_id, *version, len(image), link, sp, reset, zlib.crc32(image) & 0xFFFFFFFF, counter, key_id(ed25519.public_key(seed)))


def sign_image2(seed: bytes, header: Header2, image: bytes) -> bytes:
    """The 64-byte signature (``<image>.sig``). The key id in the header must be the key of the seed."""
    if header.key_id != key_id(ed25519.public_key(seed)):
        raise ih.ImageError("the header names another key than the seed")
    return ed25519.sign(seed, image_message2(header.pack(), image))


def decide2(blob: bytes, trusted: list[bytes], *, hw_id: int, link_address: int, floor: int, installed_version: tuple[int, int, int],
            nonce: bytes = b"", token: bytes = b"") -> str:
    """Everything a bootloader with a trust table decides about ``header || image || signature``, in order; returns the status name.

    ``trusted`` are the public keys of the table that are not revoked. The signature is checked before anything in the header is believed."""
    if len(blob) < HEADER_SIZE + ih.SIGNATURE_SIZE:
        return "too_short"
    try:
        header = parse_header2(blob)
    except ih.ImageError as error:
        return "unknown_format" if "format" in str(error) else "bad_header"
    if len(blob) != HEADER_SIZE + header.image_size + ih.SIGNATURE_SIZE:
        return "size_mismatch"
    image = blob[HEADER_SIZE:HEADER_SIZE + header.image_size]
    signature = blob[HEADER_SIZE + header.image_size:]
    keys = [k for k in trusted if key_id(k) == header.key_id]
    if not keys:
        return "unknown_key"
    if not ed25519.verify(keys[0], image_message2(blob[:HEADER_SIZE], image), signature):
        return "bad_signature"
    if zlib.crc32(image) & 0xFFFFFFFF != header.image_crc32:
        return "bad_crc"
    if header.hw_id != hw_id:
        return "hardware_mismatch"
    if header.link_address != link_address:
        return "link_mismatch"
    if len(image) < 8 or struct.unpack_from("<II", image, 0) != (header.vector_sp, header.vector_reset) or header.vector_reset & 1 == 0:
        return "bad_vectors"
    if header.rollback_counter < floor:
        return "rollback_counter"
    if header.version <= installed_version:
        digest = hashlib.sha256(blob[:HEADER_SIZE] + image).digest()
        if (header.version < installed_version and len(token) == ih.SIGNATURE_SIZE and len(nonce) == ih.NONCE_SIZE
                and ed25519.verify(keys[0], ih.downgrade_message(nonce, installed_version, header.version, digest), token)):
            return "ok"
        return "rollback_version"
    return "ok"
