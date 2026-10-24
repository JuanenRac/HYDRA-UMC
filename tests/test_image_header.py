# =============================================================================
# HYDRA-UMC - tests/test_image_header.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The signed image header and the anti-rollback rule (host reference; nothing here runs on a board)."""
import struct
import sys
import unittest
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

import image_header as ih  # noqa: E402

KEY = b"k" * 32
LINK = 0x08020000
IMAGE = struct.pack("<II", 0x20008000, LINK + 0x101) + bytes(range(256)) * 4  # 1032 bytes, with the vector table in front
HW = 0x47343031


def _header(version=(0, 1, 8), **over):
    data = dict(
        hw_id=HW, major=version[0], minor=version[1], patch=version[2], image_size=len(IMAGE),
        link_address=LINK, vector_sp=0x20008000, vector_reset=LINK + 0x101, image_crc32=zlib.crc32(IMAGE) & 0xFFFFFFFF,
    )
    data.update(over)
    return ih.ImageHeader(**data)


def _blob(version=(0, 1, 8), key=KEY, **over):
    return ih.build_signed_image(_header(version, **over), IMAGE, key)


def _verify(blob, installed=(0, 1, 7), **over):
    args = dict(hw_id=HW, link_address=LINK, installed_version=installed, ram_range=(0x20000000, 0x20008000))
    args.update(over)
    return ih.verify_image(blob, KEY, **args)


class LayoutTests(unittest.TestCase):
    def test_the_header_is_64_bytes_and_round_trips(self):
        header = _header()
        packed = header.pack()
        self.assertEqual(len(packed), 64)
        self.assertEqual(packed[:4], b"HYDR")
        self.assertEqual(ih.parse_header(packed), header)

    def test_fields_sit_at_the_documented_offsets(self):
        packed = _header((1, 2, 3)).pack()
        self.assertEqual(struct.unpack_from("<I", packed, 8)[0], HW)
        self.assertEqual(struct.unpack_from("<HHHH", packed, 12), (1, 2, 3, 0))
        self.assertEqual(struct.unpack_from("<I", packed, 20)[0], len(IMAGE))
        self.assertEqual(struct.unpack_from("<I", packed, 24)[0], LINK)
        self.assertEqual(struct.unpack_from("<IIII", packed, 28), (0x20008000, LINK + 0x101, ih.zlib.crc32(IMAGE) & 0xFFFFFFFF, 0))

    def test_bad_magic_format_and_reserved_bytes_are_refused(self):
        packed = bytearray(_header().pack())
        for offset, value in ((0, 0), (4, 9), (6, 1), (18, 1), (40, 1), (63, 1)):
            broken = bytearray(packed)
            broken[offset] = value
            with self.assertRaises(ih.ImageError):
                ih.parse_header(bytes(broken))
        with self.assertRaises(ih.ImageError):
            ih.parse_header(b"short")

    def test_a_version_number_that_does_not_fit_is_refused(self):
        with self.assertRaises(ih.ImageError):
            _header((0, 70000, 0)).pack()


class SigningTests(unittest.TestCase):
    def test_a_good_image_verifies(self):
        self.assertEqual(_verify(_blob()).version, (0, 1, 8))

    def test_the_builder_checks_size_and_crc(self):
        with self.assertRaises(ih.ImageError):
            ih.build_signed_image(_header(image_size=1), IMAGE, KEY)
        with self.assertRaises(ih.ImageError):
            ih.build_signed_image(_header(image_crc32=0), IMAGE, KEY)

    def test_every_byte_of_header_image_and_tag_is_covered(self):
        blob = _blob()
        for index in (0, 8, 12, 14, 16, 20, 24, 28, 32, 36, 64, 500, len(blob) - 33, len(blob) - 1):
            broken = bytearray(blob)
            broken[index] ^= 0x01
            with self.assertRaises(ih.ImageError, msg=f"byte {index}"):
                _verify(bytes(broken), installed=(0, 0, 1))

    def test_another_key_does_not_verify(self):
        with self.assertRaisesRegex(ih.ImageError, "tag"):
            ih.verify_image(_blob(key=b"x" * 32), KEY, hw_id=HW, link_address=LINK, installed_version=(0, 0, 1))

    def test_truncated_or_extended_data_is_refused(self):
        blob = _blob()
        for broken in (blob[:-1], blob + b"\x00", blob[:80]):
            with self.assertRaises(ih.ImageError):
                _verify(broken)


class ReplayTests(unittest.TestCase):
    def test_an_old_signed_image_cannot_claim_a_higher_version(self):
        """The old flaw: the version travelled outside what the tag covers. Now changing it breaks the tag."""
        old = bytearray(_blob(version=(0, 1, 2)))
        struct.pack_into("<HHH", old, 12, 0, 1, 9)  # claim 0.1.9
        with self.assertRaisesRegex(ih.ImageError, "tag"):
            _verify(bytes(old), installed=(0, 1, 8))


class RollbackTests(unittest.TestCase):
    def test_a_newer_patch_is_accepted(self):
        _verify(_blob((0, 1, 9)), installed=(0, 1, 8))

    def test_the_patch_counts_not_only_major_and_minor(self):
        """0.1.8 -> 0.1.2 used to pass because only major.minor were compared."""
        with self.assertRaisesRegex(ih.ImageError, "anti-rollback"):
            _verify(_blob((0, 1, 2)), installed=(0, 1, 8))

    def test_the_same_version_is_not_newer(self):
        with self.assertRaisesRegex(ih.ImageError, "anti-rollback"):
            _verify(_blob((0, 1, 8)), installed=(0, 1, 8))

    def test_minor_and_major_order_as_tuples(self):
        _verify(_blob((0, 2, 0)), installed=(0, 1, 9))
        _verify(_blob((1, 0, 0)), installed=(0, 9, 9))
        with self.assertRaises(ih.ImageError):
            _verify(_blob((0, 1, 9)), installed=(0, 2, 0))

    def test_a_downgrade_needs_a_token_made_for_this_session_and_this_pair(self):
        nonce = b"n" * 16
        blob = _blob((0, 1, 2))
        tag = blob[-ih.TAG_SIZE:]
        token = ih.downgrade_token(KEY, nonce, (0, 1, 8), (0, 1, 2), tag)
        self.assertEqual(_verify(blob, installed=(0, 1, 8), nonce=nonce, token=token).version, (0, 1, 2))
        # not for another image of the same version (another tag): the token authorizes exactly the image it was made for
        image2 = IMAGE[:-1] + b"\x00"
        header2 = _header((0, 1, 2), image_size=len(image2), image_crc32=zlib.crc32(image2) & 0xFFFFFFFF)
        blob2 = ih.build_signed_image(header2, image2, KEY)
        self.assertNotEqual(blob2[-ih.TAG_SIZE:], tag)
        with self.assertRaisesRegex(ih.ImageError, "anti-rollback"):
            _verify(blob2, installed=(0, 1, 8), nonce=nonce, token=token)
        # not with another nonce (another session), another pair, a wrong token, or none
        with self.assertRaises(ih.ImageError):
            _verify(blob, installed=(0, 1, 8), nonce=b"m" * 16, token=token)
        with self.assertRaises(ih.ImageError):
            _verify(blob, installed=(0, 1, 9), nonce=nonce, token=token)
        with self.assertRaises(ih.ImageError):
            _verify(blob, installed=(0, 1, 8), nonce=nonce, token=b"\x00" * 32)
        with self.assertRaises(ih.ImageError):
            _verify(blob, installed=(0, 1, 8))

    def test_a_token_never_allows_the_same_version(self):
        nonce = b"n" * 16
        blob = _blob((0, 1, 8))
        token = ih.downgrade_token(KEY, nonce, (0, 1, 8), (0, 1, 8), blob[-ih.TAG_SIZE:])
        with self.assertRaises(ih.ImageError):
            _verify(blob, installed=(0, 1, 8), nonce=nonce, token=token)


class PlacementTests(unittest.TestCase):
    def test_wrong_hardware_link_address_and_vectors_are_refused(self):
        with self.assertRaisesRegex(ih.ImageError, "hardware"):
            _verify(_blob(), hw_id=0x47343032)
        with self.assertRaisesRegex(ih.ImageError, "linked"):
            _verify(_blob(), link_address=LINK + 0x1000)
        with self.assertRaisesRegex(ih.ImageError, "first two words"):
            _verify(_blob(vector_reset=LINK + 0x101 + 2))
        # images whose own first words are what the header says, but are not acceptable
        for words, message in (((0x20008000, LINK + 0x100), "Thumb"), ((0x20008000, LINK + 0x10001), "outside the image"), ((0x30000000, LINK + 0x101), "RAM")):
            image = struct.pack("<II", *words) + IMAGE[8:]
            header = _header(vector_sp=words[0], vector_reset=words[1], image_size=len(image), image_crc32=zlib.crc32(image) & 0xFFFFFFFF)
            with self.assertRaisesRegex(ih.ImageError, message):
                _verify(ih.build_signed_image(header, image, KEY))


if __name__ == "__main__":
    unittest.main()
