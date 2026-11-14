# =============================================================================
# HYDRA-UMC - tests/test_fuzz_frames_and_headers.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Properties of the sealed SPI frame and of the image header format 2, held with random inputs from a fixed seed (the same run every time).

Not a replacement for the shared vectors: those fix single cases that the firmware also runs. These say what must hold for ANY input of the shape - a
frame that was built is accepted and one bit damaged in the sealed part is not, a retry is a duplicate only when the sequence repeats, a header that
parses packs back to the same bytes, random bytes never make the parser fail in an unplanned way, and no single changed byte of a signed image is
installed. Evidence: simulator."""
import hashlib
import random
import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import ed25519_ref as ed25519  # noqa: E402
import image_header as ih  # noqa: E402
import image_header2 as h2  # noqa: E402
import spi_frame  # noqa: E402

SEED = 20261114


class SealedFrame(unittest.TestCase):
    def test_a_built_frame_is_accepted_and_has_the_layout_of_the_reference(self):
        rng = random.Random(SEED)
        for _ in range(500):
            payload = bytes(rng.randrange(256) for _ in range(rng.randrange(9)))
            tier, slot, kind, seq = rng.randrange(256), rng.randrange(256), rng.randrange(256), rng.randrange(1 << 16)
            wire = spi_frame.build(tier, slot, kind, payload, seq)
            self.assertEqual(len(wire), spi_frame.FRAME_SIZE)
            self.assertEqual(spi_frame.check(wire), "ok")
            self.assertEqual((wire[0], wire[1], wire[2], wire[3]), (tier, slot, kind, len(payload)))
            self.assertEqual(wire[4:4 + len(payload)], payload)
            self.assertEqual(wire[4 + len(payload):12], bytes(8 - len(payload)))
            self.assertEqual(wire[spi_frame.OFS_SEQ], seq & 0xFF)
            self.assertEqual(wire[16:], b"\xff" * (spi_frame.FRAME_SIZE - 16))

    def test_up_to_three_damaged_bits_in_the_sealed_part_are_never_taken_for_good(self):
        rng = random.Random(SEED + 1)
        sealed_bits = 8 * (spi_frame.OFS_CRC + 2)
        for _ in range(2000):
            payload = bytes(rng.randrange(256) for _ in range(rng.randrange(9)))
            wire = spi_frame.build(rng.randrange(256), rng.randrange(256), rng.randrange(256), payload, rng.randrange(256))
            damaged = bytearray(wire)
            for bit in rng.sample(range(sealed_bits), rng.choice((1, 2, 3))):
                damaged[bit // 8] ^= 1 << (bit % 8)
            self.assertNotEqual(spi_frame.check(bytes(damaged)), "ok")

    def test_the_padding_after_the_crc_is_not_covered_and_that_is_the_design(self):
        wire = spi_frame.build(2, 1, 0x10, b"\x01", 9)
        rng = random.Random(SEED + 2)
        for _ in range(200):
            damaged = bytearray(wire)
            damaged[rng.randrange(16, spi_frame.FRAME_SIZE)] ^= 1 << rng.randrange(8)
            self.assertEqual(spi_frame.check(bytes(damaged)), "ok")

    def test_a_frame_of_the_wrong_size_is_refused_not_judged(self):
        rng = random.Random(SEED + 3)
        for size in (0, 1, 15, 16, 127, 129, 256):
            with self.assertRaises(ValueError):
                spi_frame.check(bytes(rng.randrange(256) for _ in range(size)))
        with self.assertRaises(ValueError):
            spi_frame.build(0, 0, 0, bytes(9), 1)

    def test_a_receiver_dispatches_a_frame_unless_the_sequence_repeats(self):
        rng = random.Random(SEED + 4)
        last = None
        dispatched, expected = 0, 0
        previous = None
        for _ in range(3000):
            seq = previous if (previous is not None and rng.random() < 0.3) else rng.randrange(256)
            wire = spi_frame.build(1, 2, 3, b"", seq)
            verdict = spi_frame.judge(wire, last)
            if seq == previous:
                self.assertEqual(verdict, "duplicate")
            else:
                self.assertEqual(verdict, "dispatch")
                expected += 1
            if verdict == "dispatch":
                dispatched += 1
                last = wire[spi_frame.OFS_SEQ]
            previous = seq
        self.assertEqual(dispatched, expected)

    def test_a_damaged_frame_never_moves_the_last_sequence(self):
        wire = bytearray(spi_frame.build(1, 2, 3, b"\x07", 41))
        wire[5] ^= 0x10
        self.assertEqual(spi_frame.judge(bytes(wire), 41), "bad_crc")
        self.assertEqual(spi_frame.judge(bytes(wire), None), "bad_crc")
        self.assertEqual(spi_frame.judge(spi_frame.build(1, 2, 3, b"\x07"), 41), "unsealed")


class HeaderFormat2(unittest.TestCase):
    def test_a_header_packs_back_to_the_bytes_it_was_parsed_from(self):
        rng = random.Random(SEED + 5)
        for _ in range(1000):
            header = h2.Header2(
                rng.getrandbits(32), rng.getrandbits(16), rng.getrandbits(16), rng.getrandbits(16), rng.getrandbits(32), rng.getrandbits(32),
                rng.getrandbits(32), rng.getrandbits(32), rng.getrandbits(32), rng.getrandbits(32), bytes(rng.getrandbits(8) for _ in range(8)),
            )
            packed = header.pack()
            self.assertEqual(len(packed), h2.HEADER_SIZE)
            self.assertEqual(h2.parse_header2(packed), header)
            self.assertEqual(h2.parse_header2(packed + bytes(rng.getrandbits(8) for _ in range(rng.randrange(64)))), header)

    def test_a_field_out_of_range_is_refused_when_packing(self):
        good = dict(hw_id=1, major=1, minor=2, patch=3, image_size=64, link_address=0x08009000, vector_sp=0x20010000, vector_reset=0x08009101,
                    image_crc32=7, rollback_counter=1, key_id=bytes(8))
        h2.Header2(**good).pack()
        for name, value in (("major", 0x10000), ("minor", -1), ("patch", 0x10000), ("hw_id", 1 << 32), ("image_size", -1), ("link_address", 1 << 32),
                            ("vector_sp", 1 << 32), ("vector_reset", -1), ("image_crc32", 1 << 32), ("rollback_counter", 1 << 32)):
            with self.assertRaises(ih.ImageError, msg=name):
                h2.Header2(**{**good, name: value}).pack()
        for length in (0, 7, 9):
            with self.assertRaises(ih.ImageError):
                h2.Header2(**{**good, "key_id": bytes(length)}).pack()

    def test_random_bytes_make_the_parser_say_no_in_its_own_words_or_yes_exactly(self):
        rng = random.Random(SEED + 6)
        good = h2.Header2(1, 1, 2, 3, 64, 0x08009000, 0x20010000, 0x08009101, 7, 1, bytes(8)).pack()
        parsed = 0
        for index in range(4000):
            if index % 3 == 0:
                data = bytes(rng.getrandbits(8) for _ in range(rng.randrange(0, 2 * h2.HEADER_SIZE)))
            else:                                                  # a valid header with a few bytes changed: gets past the magic and the format
                damaged = bytearray(good)
                for _ in range(rng.randrange(1, 4)):
                    damaged[rng.randrange(len(damaged))] = rng.getrandbits(8)
                data = bytes(damaged)
            try:
                header = h2.parse_header2(data)
            except ih.ImageError:
                continue
            parsed += 1
            self.assertEqual(header.pack(), data[:h2.HEADER_SIZE])
        self.assertGreater(parsed, 0)

    def test_random_blobs_always_get_a_named_verdict_and_never_install(self):
        rng = random.Random(SEED + 7)
        public = ed25519.public_key(hashlib.sha256(b"fuzz verdict key").digest())
        for _ in range(300):
            blob = bytes(rng.getrandbits(8) for _ in range(rng.randrange(0, 400)))
            verdict = h2.decide2(blob, [public], hw_id=1, link_address=0x08009000, floor=0, installed_version=(0, 0, 0))
            self.assertIn(verdict, h2.STATUS)
            self.assertNotEqual(verdict, "ok")

    def test_no_single_changed_byte_of_a_signed_image_installs(self):
        rng = random.Random(SEED + 8)
        seed = hashlib.sha256(b"fuzz signing key").digest()
        public = ed25519.public_key(seed)
        image = struct.pack("<II", 0x20010000, 0x08009101) + bytes(rng.getrandbits(8) for _ in range(56))
        header = h2.make_header2(seed, image, 0x47343031, (1, 2, 3), 0x08009000, 5)
        blob = header.pack() + image + h2.sign_image2(seed, header, image)
        arguments = dict(hw_id=0x47343031, link_address=0x08009000, floor=5, installed_version=(1, 2, 2))
        self.assertEqual(h2.decide2(blob, [public], **arguments), "ok")
        positions = sorted(rng.sample(range(len(blob)), 24)) + [0, h2.HEADER_SIZE - 1, h2.HEADER_SIZE, len(blob) - 1]
        for position in positions:
            damaged = bytearray(blob)
            damaged[position] ^= 1 << rng.randrange(8)
            self.assertNotEqual(h2.decide2(bytes(damaged), [public], **arguments), "ok", position)

    def test_a_blob_cut_or_extended_never_installs(self):
        seed = hashlib.sha256(b"fuzz signing key").digest()
        public = ed25519.public_key(seed)
        image = struct.pack("<II", 0x20010000, 0x08009101) + bytes(range(56))
        header = h2.make_header2(seed, image, 0x47343031, (1, 2, 3), 0x08009000, 5)
        blob = header.pack() + image + h2.sign_image2(seed, header, image)
        arguments = dict(hw_id=0x47343031, link_address=0x08009000, floor=5, installed_version=(1, 2, 2))
        for length in (0, 1, h2.HEADER_SIZE - 1, h2.HEADER_SIZE, h2.HEADER_SIZE + ih.SIGNATURE_SIZE - 1, len(blob) - 1):
            self.assertIn(h2.decide2(blob[:length], [public], **arguments), ("too_short", "size_mismatch"), length)
        self.assertEqual(h2.decide2(blob + b"\x00", [public], **arguments), "size_mismatch")


if __name__ == "__main__":
    unittest.main()
