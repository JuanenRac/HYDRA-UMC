#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/image_header.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Reference implementation (host side) of the signed firmware image header and the anti-rollback rule.

Why this exists: the update protocol used to carry the new version in END_UPDATE, outside the data the HMAC covers, and compared only
major.minor, so an old signed image could be replayed declaring a higher version, and 0.1.8 -> 0.1.2 was not noticed. Here the version
(major, minor, patch), the hardware id, the link address, the size, the vector words and the CRC of the image are inside a fixed header,
and the tag covers header and image together, so none of them can be changed without breaking the tag. The rule "a new image must
be strictly newer than the installed one" compares the three numbers.

This module does not touch any firmware: it fixes the byte layout and the decisions with test vectors, so that the bootloader
(C) can be written against something that is already checked. Nothing here has run on a board.

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
    ...  tag   HMAC-SHA256 over header || image (32 bytes) follows the image

The tag here is the HMAC the bootloader already checks. An asymmetric signature (Ed25519) is the planned replacement, so that the
board holds no secret that signs: only the 32-byte tag field and the verify call change; the header stays as it is.
"""
from __future__ import annotations

import hashlib
import hmac
import struct
import zlib
from dataclasses import dataclass

MAGIC = 0x52445948
FORMAT = 1
HEADER_SIZE = 64
TAG_SIZE = 32
NONCE_SIZE = 16
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


def _tag(key: bytes, header_bytes: bytes, image: bytes) -> bytes:
    return hmac.new(key, header_bytes + image, hashlib.sha256).digest()


def build_signed_image(header: ImageHeader, image: bytes, key: bytes) -> bytes:
    """header || image || tag, with the size and CRC of the header checked against the image."""
    if header.image_size != len(image):
        raise ImageError("image_size does not match the image")
    if header.image_crc32 != zlib.crc32(image) & 0xFFFFFFFF:
        raise ImageError("image_crc32 does not match the image")
    packed = header.pack()
    return packed + image + _tag(key, packed, image)


def downgrade_token(key: bytes, nonce: bytes, installed: tuple[int, int, int], target: tuple[int, int, int], image_tag: bytes) -> bytes:
    """Authorisation to go from `installed` to the older `target` with the image whose tag is `image_tag`: derived from the key and a
    nonce the bootloader chose for this session, so it is useless on any other session, version pair, image or board. Not a constant
    in the firmware."""
    if len(nonce) != NONCE_SIZE:
        raise ImageError(f"the session nonce is {NONCE_SIZE} bytes, got {len(nonce)}")
    if len(image_tag) != TAG_SIZE:
        raise ImageError(f"the tag of the image is {TAG_SIZE} bytes, got {len(image_tag)}")
    message = b"hydra-umc-downgrade\x00" + nonce + struct.pack("<3H3H", *installed, *target) + image_tag
    return hmac.new(key, message, hashlib.sha256).digest()


def verify_image(
    blob: bytes,
    key: bytes,
    *,
    hw_id: int,
    link_address: int,
    installed_version: tuple[int, int, int],
    ram_range: tuple[int, int] | None = None,
    nonce: bytes = b"",
    token: bytes = b"",
) -> ImageHeader:
    """Everything the bootloader decides, in the order it should decide it. Returns the header when the image may be installed."""
    if len(blob) < HEADER_SIZE + TAG_SIZE:
        raise ImageError("the image is shorter than a header and a tag")
    header = parse_header(blob)
    if len(blob) != HEADER_SIZE + header.image_size + TAG_SIZE:
        raise ImageError("the size in the header does not match the data received")
    header_bytes = blob[:HEADER_SIZE]
    image = blob[HEADER_SIZE:HEADER_SIZE + header.image_size]
    tag = blob[HEADER_SIZE + header.image_size:]
    # the tag first: nothing in the header is believed before it verifies
    if not hmac.compare_digest(tag, _tag(key, header_bytes, image)):
        raise ImageError("the tag does not verify: the image or its header was changed or signed with another key")
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
        # older or equal: only with a token made for exactly this pair of versions in this session
        if header.version < installed_version and token and len(nonce) == NONCE_SIZE and hmac.compare_digest(
            token, downgrade_token(key, nonce, installed_version, header.version, tag)
        ):
            return header
        raise ImageError(
            f"anti-rollback: {'.'.join(map(str, header.version))} is not newer than the installed {'.'.join(map(str, installed_version))}"
        )
    return header
