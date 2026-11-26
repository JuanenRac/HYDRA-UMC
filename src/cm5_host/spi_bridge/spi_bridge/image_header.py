# =============================================================================
# HYDRA-UMC - CM5 side of the signed firmware image header
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see repo root LICENSE
# =============================================================================
"""What the CM5 sends so that a HYDRA-UMC bootloader accepts an update: the 64-byte header of format 2 (the version with its patch, the
hardware id, the size, the link address, the vectors and the CRC of the image, the rollback counter and the id of the signing key), the
Ed25519 signature of the digest of header || image and, for a downgrade, a second signature (the token).

The bootloaders read format 2 only. The CM5 never holds a signing key: the signature is made by the owner on a machine of their own and
handed to the flasher together with the header it covers (<image>.hdr2 of a release), or made on demand by a callable the caller
supplies. The rollback counter and the key id are part of the signed bytes and this side cannot guess them: they come with the header, or
are given one by one. All this module does is build or check the header and make the two messages that are signed, byte for byte as
tools/image_header2.py (the host reference of the repository) and src/*/boot/image_header.h and update_verify.h do.
tests/test_signed_flash.py compares what this module builds with that reference, and tests/test_header_against_bootloader.py gives it to
the parser and the decision of the three bootloaders, compiled for the host.

Only the HYDRA-UMC bootloaders (the Robot Controller Board and the two cores of the Kinematic Brain) take this format. The tool heads and
expansion boards behind the relay run the bootloaders of URTC, which still take the image without a header: see is_signed_header_target.
"""

from __future__ import annotations

import hashlib
import struct
import zlib
from collections.abc import Callable

MAGIC = 0x52445948  # "HYDR"
FORMAT = 2
HEADER_SIZE = 64
KEY_ID_SIZE = 8
DIGEST_SIZE = 32
SIGNATURE_SIZE = 64
NONCE_SIZE = 16
HEADER_CHUNKS = 8
SIGNATURE_CHUNKS = 8
TOKEN_CHUNKS = 8
# The two things that are signed start with a different text, so a signature made for one can never pass for the other.
IMAGE_DOMAIN = b"hydra-umc-image-sig/2\x00"
DOWNGRADE_DOMAIN = b"hydra-umc-downgrade\x00"

# Signs the digest message of an image (header, firmware, digest -> 64 bytes) / the message of a downgrade (nonce, installed, target, digest -> 64 bytes).
ImageSigner = Callable[[bytes, bytes, bytes], bytes]
DowngradeSigner = Callable[[bytes, tuple[int, int, int], tuple[int, int, int], bytes], bytes]

_LAYOUT = struct.Struct("<IHHIHHHHIIIIII8s12s")
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


def build_header(hardware_id: int, version: tuple[int, int, int], firmware: bytes, rollback_counter: int, key_id: bytes) -> bytes:
    """The 64-byte header of `firmware` for the bootloader with this hardware id. The vector words are the first two words of the image.
    `rollback_counter` is the number the bootloader compares with the floor it keeps, and `key_id` (8 bytes, the start of the SHA-512 of
    the public key) names the key of its trust table that made the signature: both are what the image was signed with."""
    if hardware_id not in LINK_ADDRESS_BY_HARDWARE_ID:
        raise ImageHeaderError(f"hardware id 0x{hardware_id:08X} is not a HYDRA-UMC bootloader that takes a signed header")
    if len(firmware) < 8:
        raise ImageHeaderError("the image is too short to hold its vector table")
    for name, value in zip(("major", "minor", "patch"), version, strict=True):
        if not 0 <= value <= 0xFFFF:
            raise ImageHeaderError(f"the {name} of the version must fit in 16 bits, got {value}")
    if not 0 <= rollback_counter <= 0xFFFFFFFF:
        raise ImageHeaderError(f"the rollback counter must fit in 32 bits, got {rollback_counter}")
    if len(key_id) != KEY_ID_SIZE:
        raise ImageHeaderError(f"the id of the signing key is {KEY_ID_SIZE} bytes, got {len(key_id)}")
    stack, reset = struct.unpack_from("<II", firmware, 0)
    return _LAYOUT.pack(
        MAGIC, FORMAT, 0, hardware_id, version[0], version[1], version[2], 0,
        len(firmware), LINK_ADDRESS_BY_HARDWARE_ID[hardware_id], stack, reset, zlib.crc32(firmware) & 0xFFFFFFFF,
        rollback_counter, key_id, bytes(12),
    )


def check_header(header: bytes, hardware_id: int, version: tuple[int, int, int], firmware: bytes) -> None:
    """Refuses a header made elsewhere (<image>.hdr2) that is not a format 2 header of exactly this image, board and version: the board
    would only say so after the whole transfer."""
    if len(header) != HEADER_SIZE:
        raise ImageHeaderError(f"the header is {HEADER_SIZE} bytes, got {len(header)}")
    fields = _LAYOUT.unpack(header)
    if fields[0] != MAGIC or fields[1] != FORMAT:
        raise ImageHeaderError(f"the header is not a HYDRA-UMC header of format {FORMAT}")
    if header != build_header(hardware_id, version, firmware, fields[13], fields[14]):
        raise ImageHeaderError("the header does not describe this image, board and version")


def image_digest(header: bytes, firmware: bytes) -> bytes:
    """SHA-256 over header || image: what the bootloader hashes out of its flash while the image arrives."""
    return hashlib.sha256(header + firmware).digest()


def image_message(digest: bytes) -> bytes:
    """The message the signature of an image is made over: the domain text, then the digest."""
    if len(digest) != DIGEST_SIZE:
        raise ImageHeaderError(f"the digest of an image is {DIGEST_SIZE} bytes, got {len(digest)}")
    return IMAGE_DOMAIN + digest


def downgrade_message(nonce: bytes, installed: tuple[int, int, int], target: tuple[int, int, int], digest: bytes) -> bytes:
    """The message whose signature allows going from `installed` to the older `target` with the image whose digest is `digest`, in the
    session `nonce` belongs to. The bootloader builds the same message and verifies the signature; it holds no constant that allows a
    downgrade and no key that makes one."""
    if len(nonce) != NONCE_SIZE:
        raise ImageHeaderError(f"the nonce of the session is {NONCE_SIZE} bytes, got {len(nonce)}")
    if len(digest) != DIGEST_SIZE:
        raise ImageHeaderError(f"the digest of the image is {DIGEST_SIZE} bytes, got {len(digest)}")
    return DOWNGRADE_DOMAIN + nonce + struct.pack("<3H3H", *installed, *target) + digest
