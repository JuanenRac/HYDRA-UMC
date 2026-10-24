# =============================================================================
# HYDRA-UMC - CM5 side of the signed firmware image header
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see repo root LICENSE
# =============================================================================
"""What the CM5 sends so that a HYDRA-UMC bootloader accepts an update: the 64-byte header (the version with its patch, the hardware id,
the size, the link address, the vectors and the CRC of the image), the tag (HMAC-SHA256 over header || image) and, for a downgrade, the token.

The byte layout and the rules are those of tools/image_header.py (the host reference of the repository, whose test vectors the C code of the
three bootloaders is checked against) and of src/*/boot/image_header.h and update_verify.h. tests/test_signed_flash.py compares what this
module builds with that reference and runs it against a bootloader model that applies the reference's own verification.

Only the HYDRA-UMC bootloaders (the Robot Controller Board and the two cores of the Kinematic Brain) take this format. The tool heads and
expansion boards behind the relay run the bootloaders of URTC, which still take the image without a header: see is_signed_header_target.
"""

from __future__ import annotations

import hashlib
import hmac
import struct
import zlib

MAGIC = 0x52445948  # "HYDR"
FORMAT = 1
HEADER_SIZE = 64
TAG_SIZE = 32
NONCE_SIZE = 16
HEADER_CHUNKS = 8
TAG_CHUNKS = 4
TOKEN_CHUNKS = 4

_LAYOUT = struct.Struct("<IHHIHHHHIIIII24s")
assert _LAYOUT.size == HEADER_SIZE

# hardware id -> where that bootloader links its application (MAIN_APP_ADDR of its bootloader_common.h)
LINK_ADDRESS_BY_HARDWARE_ID = {
    0x47343031: 0x08009000,  # "G401": STM32G474, Robot Controller Board
    0x48374334: 0x08140000,  # "H7C4": STM32H745 CM4, Kinematic Brain
    0x48374337: 0x08040000,  # "H7C7": STM32H745 CM7, Kinematic Brain
}


class ImageHeaderError(ValueError):
    """The image or its description cannot be turned into a signed update."""


def is_signed_header_target(hardware_id: int) -> bool:
    """True for the bootloaders that take the signed header (the HYDRA-UMC ones); False for the URTC ones behind the relay."""
    return hardware_id in LINK_ADDRESS_BY_HARDWARE_ID


def build_header(hardware_id: int, version: tuple[int, int, int], firmware: bytes) -> bytes:
    """The 64-byte header of `firmware` for the bootloader with this hardware id. The vector words are the first two words of the image."""
    if hardware_id not in LINK_ADDRESS_BY_HARDWARE_ID:
        raise ImageHeaderError(f"hardware id 0x{hardware_id:08X} is not a HYDRA-UMC bootloader that takes a signed header")
    if len(firmware) < 8:
        raise ImageHeaderError("the image is too short to hold its vector table")
    for name, value in zip(("major", "minor", "patch"), version, strict=True):
        if not 0 <= value <= 0xFFFF:
            raise ImageHeaderError(f"the {name} of the version must fit in 16 bits, got {value}")
    stack, reset = struct.unpack_from("<II", firmware, 0)
    return _LAYOUT.pack(
        MAGIC, FORMAT, 0, hardware_id, version[0], version[1], version[2], 0,
        len(firmware), LINK_ADDRESS_BY_HARDWARE_ID[hardware_id], stack, reset, zlib.crc32(firmware) & 0xFFFFFFFF, b"\x00" * 24,
    )


def tag_of(key: bytes, header: bytes, firmware: bytes) -> bytes:
    """HMAC-SHA256 over header || image: what the bootloader recomputes from its flash."""
    return hmac.new(key, header + firmware, hashlib.sha256).digest()


def downgrade_token(key: bytes, nonce: bytes, installed: tuple[int, int, int], target: tuple[int, int, int], image_tag: bytes) -> bytes:
    """The token that allows going from `installed` to the older `target` with the image whose tag is `image_tag`, in the session `nonce`
    belongs to. The bootloader computes the same value and compares; it holds no constant that allows a downgrade."""
    if len(nonce) != NONCE_SIZE:
        raise ImageHeaderError(f"the nonce of the session is {NONCE_SIZE} bytes, got {len(nonce)}")
    if len(image_tag) != TAG_SIZE:
        raise ImageHeaderError(f"the tag of the image is {TAG_SIZE} bytes, got {len(image_tag)}")
    message = b"hydra-umc-downgrade\x00" + nonce + struct.pack("<3H3H", *installed, *target) + image_tag
    return hmac.new(key, message, hashlib.sha256).digest()
