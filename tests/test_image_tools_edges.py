# =============================================================================
# HYDRA-UMC - tests/test_image_tools_edges.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Edges and command lines of the image header and trust table tools: every refusal, the signing and trust commands end to end, and seeded mutation sweeps.

The host reference of the bootloader decision must refuse every doubt; these tests walk each refusal one by one, then flip bytes of a good image or table
(with a fixed seed) and require that nothing altered is ever accepted. Nothing here touches a board, and every file is written in a throw-away folder.
"""
import contextlib
import copy
import io
import json
import random
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

import ed25519_ref  # noqa: E402
import image_header as ih  # noqa: E402
import image_trust as it  # noqa: E402

SEED = bytes(range(32))
OTHER_SEED = bytes(range(1, 33))
PUBLIC = ed25519_ref.public_key(SEED)
OTHER_PUBLIC = ed25519_ref.public_key(OTHER_SEED)
LINK = 0x08020000
SP = 0x20008000
HW = 0x47343031
IMAGE = struct.pack("<II", SP, LINK + 0x41) + bytes((i * 7) & 0xFF for i in range(120))
RAM = (0x20000000, 0x20010000)


def header(version=(1, 2, 3), image=IMAGE, **over) -> ih.ImageHeader:
    data = dict(
        hw_id=HW, major=version[0], minor=version[1], patch=version[2], image_size=len(image), link_address=LINK,
        vector_sp=SP, vector_reset=LINK + 0x41, image_crc32=zlib.crc32(image) & 0xFFFFFFFF,
    )
    data.update(over)
    return ih.ImageHeader(**data)


def signed_unchecked(hdr: ih.ImageHeader, image: bytes, seed: bytes = SEED) -> bytes:
    """A blob signed as it is, without the consistency checks of build_signed_image (to build the images a bootloader must still refuse)."""
    packed = hdr.pack()
    return packed + image + ih.sign_image(seed, packed, image)


def verify(blob: bytes, **over) -> ih.ImageHeader:
    args = dict(hw_id=HW, link_address=LINK, installed_version=(1, 2, 2), ram_range=RAM)
    args.update(over)
    return ih.verify_image(blob, PUBLIC, **args)


def run(module, argv):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = module.main(argv)
    return code, out.getvalue(), err.getvalue()


class HeaderFieldTests(unittest.TestCase):
    def test_a_version_part_over_16_bits_is_refused_when_packing(self):
        for name in ("major", "minor", "patch"):
            with self.assertRaisesRegex(ih.ImageError, name):
                header(**{name: 0x10000}).pack()
            with self.assertRaisesRegex(ih.ImageError, name):
                header(**{name: -1}).pack()
        header(major=0xFFFF, minor=0xFFFF, patch=0xFFFF).pack()  # the largest legal one

    def test_a_32_bit_field_out_of_range_is_refused_when_packing(self):
        for name in ("hw_id", "image_size", "link_address", "vector_sp", "vector_reset", "image_crc32"):
            with self.assertRaisesRegex(ih.ImageError, name):
                header(**{name: 1 << 32}).pack()
            with self.assertRaisesRegex(ih.ImageError, name):
                header(**{name: -1}).pack()

    def test_parse_refuses_a_short_buffer_at_every_length(self):
        packed = header().pack()
        for length in range(ih.HEADER_SIZE):
            with self.assertRaisesRegex(ih.ImageError, "shorter than its header"):
                ih.parse_header(packed[:length])

    def test_parse_refuses_a_wrong_magic_and_an_unknown_format(self):
        packed = bytearray(header().pack())
        wrong_magic = bytearray(packed)
        wrong_magic[0] ^= 0xFF
        with self.assertRaisesRegex(ih.ImageError, "bad magic"):
            ih.parse_header(bytes(wrong_magic))
        packed[4] = ih.FORMAT + 1
        with self.assertRaisesRegex(ih.ImageError, "unknown header format"):
            ih.parse_header(bytes(packed))

    def test_parse_refuses_any_nonzero_reserved_byte(self):
        packed = header().pack()
        # flags (6-7), the reserved halfword after the patch (18-19) and the 24 padding bytes (40-63)
        for offset in [6, 7, 18, 19, *range(40, 64)]:
            changed = bytearray(packed)
            changed[offset] = 1
            with self.assertRaisesRegex(ih.ImageError, "reserved"):
                ih.parse_header(bytes(changed))

    def test_a_header_followed_by_more_bytes_parses_the_same(self):
        self.assertEqual(ih.parse_header(header().pack() + b"tail"), header())

    def test_the_version_is_a_tuple_that_orders_like_a_version(self):
        self.assertEqual(header((1, 2, 3)).version, (1, 2, 3))
        self.assertLess(header((1, 2, 9)).version, header((1, 10, 0)).version)

    def test_the_message_and_digest_helpers_check_their_sizes(self):
        with self.assertRaisesRegex(ih.ImageError, "digest"):
            ih.image_message(b"short")
        self.assertEqual(len(ih.image_message(bytes(32))), len(ih.IMAGE_DOMAIN) + 32)
        self.assertEqual(len(ih.image_digest(b"h", b"i")), 32)
        self.assertEqual(ih.image_digest(b"ab", b"c"), ih.image_digest(b"a", b"bc"), "the digest is over header || image, so the split is not part of it")

    def test_the_downgrade_message_checks_the_nonce_and_the_digest(self):
        with self.assertRaisesRegex(ih.ImageError, "nonce"):
            ih.downgrade_message(b"x" * 15, (1, 0, 0), (0, 9, 0), bytes(32))
        with self.assertRaisesRegex(ih.ImageError, "digest"):
            ih.downgrade_message(bytes(16), (1, 0, 0), (0, 9, 0), bytes(31))
        message = ih.downgrade_message(bytes(16), (1, 0, 0), (0, 9, 0), bytes(32))
        self.assertTrue(message.startswith(ih.DOWNGRADE_DOMAIN))
        self.assertEqual(len(message), len(ih.DOWNGRADE_DOMAIN) + 16 + 12 + 32)

    def test_a_downgrade_message_differs_when_any_part_differs(self):
        base = ih.downgrade_message(bytes(16), (1, 0, 0), (0, 9, 0), bytes(32))
        for changed in (
            ih.downgrade_message(b"\x01" + bytes(15), (1, 0, 0), (0, 9, 0), bytes(32)),
            ih.downgrade_message(bytes(16), (1, 0, 1), (0, 9, 0), bytes(32)),
            ih.downgrade_message(bytes(16), (1, 0, 0), (0, 8, 0), bytes(32)),
            ih.downgrade_message(bytes(16), (1, 0, 0), (0, 9, 0), b"\x01" + bytes(31)),
        ):
            self.assertNotEqual(base, changed)

    def test_build_signed_image_refuses_a_size_or_crc_that_does_not_match(self):
        with self.assertRaisesRegex(ih.ImageError, "image_size"):
            ih.build_signed_image(header(image_size=len(IMAGE) + 1), IMAGE, SEED)
        with self.assertRaisesRegex(ih.ImageError, "image_crc32"):
            ih.build_signed_image(header(image_crc32=1), IMAGE, SEED)
        blob = ih.build_signed_image(header(), IMAGE, SEED)
        self.assertEqual(len(blob), ih.HEADER_SIZE + len(IMAGE) + ih.SIGNATURE_SIZE)

    def test_c_initializer_is_upper_case_hex_with_commas(self):
        self.assertEqual(ih.c_initializer(b"\x00\xab\xff"), "0x00, 0xAB, 0xFF")
        self.assertEqual(ih.c_initializer(b""), "")


class VerifyImageTests(unittest.TestCase):
    def setUp(self):
        self.blob = ih.build_signed_image(header(), IMAGE, SEED)

    def test_the_good_image_is_accepted_and_its_header_returned(self):
        self.assertEqual(verify(self.blob), header())

    def test_a_blob_shorter_than_header_and_signature_is_refused_at_every_length(self):
        for length in range(0, ih.HEADER_SIZE + ih.SIGNATURE_SIZE):
            with self.assertRaises(ih.ImageError):
                verify(self.blob[:length])

    def test_a_truncated_or_extended_blob_does_not_match_the_size_in_its_header(self):
        for blob in (self.blob[:-1], self.blob + b"\x00", self.blob[:-65]):
            with self.assertRaises(ih.ImageError):
                verify(blob)

    def test_a_signature_from_another_key_is_refused(self):
        other = ih.build_signed_image(header(), IMAGE, OTHER_SEED)
        with self.assertRaisesRegex(ih.ImageError, "signature does not verify"):
            verify(other)

    def test_the_signature_is_checked_before_anything_in_the_header_is_believed(self):
        # wrong hardware AND a bad signature: the signature is what is reported
        other = signed_unchecked(header(hw_id=0x12345678), IMAGE, OTHER_SEED)
        with self.assertRaisesRegex(ih.ImageError, "signature"):
            verify(other)

    def test_a_signed_image_whose_crc_in_the_header_is_wrong_is_refused(self):
        with self.assertRaisesRegex(ih.ImageError, "CRC"):
            verify(signed_unchecked(header(image_crc32=0xDEADBEEF), IMAGE))

    def test_an_image_for_another_board_or_another_address_is_refused(self):
        with self.assertRaisesRegex(ih.ImageError, "hardware"):
            verify(self.blob, hw_id=0x48374337)
        with self.assertRaisesRegex(ih.ImageError, "another address"):
            verify(self.blob, link_address=LINK + 0x400)

    def test_vectors_that_disagree_with_the_image_are_refused(self):
        with self.assertRaisesRegex(ih.ImageError, "vectors of the header"):
            verify(signed_unchecked(header(vector_sp=SP + 4), IMAGE))
        with self.assertRaisesRegex(ih.ImageError, "vectors of the header"):
            verify(signed_unchecked(header(vector_reset=LINK + 0x43), IMAGE))

    def test_an_image_too_small_to_hold_a_vector_table_is_refused(self):
        tiny = b"\x01\x02\x03"
        blob = signed_unchecked(header(image=tiny, vector_sp=0, vector_reset=0), tiny)
        with self.assertRaisesRegex(ih.ImageError, "vectors of the header"):
            verify(blob)

    def test_a_reset_vector_without_the_thumb_bit_is_refused(self):
        image = struct.pack("<II", SP, LINK + 0x40) + IMAGE[8:]
        blob = signed_unchecked(header(image=image, vector_reset=LINK + 0x40), image)
        with self.assertRaisesRegex(ih.ImageError, "Thumb"):
            verify(blob)

    def test_a_reset_vector_outside_the_image_is_refused_on_both_sides(self):
        for target in (LINK - 2 + 1, LINK + len(IMAGE) + 1, LINK + 0x4001):
            image = struct.pack("<II", SP, target) + IMAGE[8:]
            blob = signed_unchecked(header(image=image, vector_reset=target), image)
            with self.assertRaisesRegex(ih.ImageError, "outside the image"):
                verify(blob)

    def test_the_last_byte_of_the_image_is_a_legal_reset_target(self):
        target = (LINK + len(IMAGE) - 2) | 1
        image = struct.pack("<II", SP, target) + IMAGE[8:]
        self.assertEqual(verify(signed_unchecked(header(image=image, vector_reset=target), image)).vector_reset, target)

    def test_the_stack_pointer_must_lie_inside_the_ram_range_when_one_is_given(self):
        with self.assertRaisesRegex(ih.ImageError, "stack pointer"):
            verify(self.blob, ram_range=(0x20000000, SP - 1))
        with self.assertRaisesRegex(ih.ImageError, "stack pointer"):
            verify(self.blob, ram_range=(SP + 1, SP + 0x100))
        verify(self.blob, ram_range=(SP, SP))           # both ends are inside
        verify(self.blob, ram_range=None)               # no range: not checked

    def test_an_equal_or_older_version_is_refused_without_a_token(self):
        for installed in ((1, 2, 3), (1, 2, 4), (1, 3, 0), (2, 0, 0)):
            with self.assertRaisesRegex(ih.ImageError, "anti-rollback"):
                verify(self.blob, installed_version=installed)

    def test_the_versions_compare_as_numbers_not_as_text(self):
        newer = ih.build_signed_image(header((1, 10, 0)), IMAGE, SEED)
        verify(newer, installed_version=(1, 9, 99))
        with self.assertRaisesRegex(ih.ImageError, "anti-rollback"):
            verify(ih.build_signed_image(header((1, 9, 0)), IMAGE, SEED), installed_version=(1, 10, 0))

    def _token(self, nonce=bytes(range(16)), installed=(1, 2, 4), target=(1, 2, 3), seed=SEED):
        digest = ih.image_digest(self.blob[:ih.HEADER_SIZE], IMAGE)
        return ih.downgrade_token(seed, nonce, installed, target, digest)

    def test_a_downgrade_needs_a_token_made_for_exactly_this_pair_and_session(self):
        nonce = bytes(range(16))
        good = self._token()
        self.assertEqual(verify(self.blob, installed_version=(1, 2, 4), nonce=nonce, token=good), header())
        cases = {
            "another session": dict(nonce=bytes(16), token=good),
            "another installed version": dict(installed_version=(1, 2, 5), nonce=nonce, token=good),
            "a token for another key": dict(nonce=nonce, token=self._token(seed=OTHER_SEED)),
            "a token of the wrong size": dict(nonce=nonce, token=good[:-1]),
            "a nonce of the wrong size": dict(nonce=nonce[:-1], token=good),
            "no token": dict(nonce=nonce, token=b""),
            "no nonce": dict(nonce=b"", token=good),
        }
        for name, extra in cases.items():
            args = {"installed_version": (1, 2, 4), **extra}
            with self.subTest(name), self.assertRaisesRegex(ih.ImageError, "anti-rollback"):
                verify(self.blob, **args)

    def test_a_token_never_lets_the_same_version_in_again(self):
        nonce = bytes(range(16))
        token = self._token(installed=(1, 2, 3), target=(1, 2, 3))
        with self.assertRaisesRegex(ih.ImageError, "anti-rollback"):
            verify(self.blob, installed_version=(1, 2, 3), nonce=nonce, token=token)

    def test_a_token_for_one_image_does_not_authorise_another(self):
        nonce = bytes(range(16))
        token = self._token()
        other_image = IMAGE[:-1] + b"\xAA"
        other = ih.build_signed_image(header(image=other_image), other_image, SEED)
        with self.assertRaisesRegex(ih.ImageError, "anti-rollback"):
            verify(other, installed_version=(1, 2, 4), nonce=nonce, token=token)

    def test_no_single_byte_change_of_a_good_image_is_ever_accepted(self):
        rng = random.Random(20240611)
        positions = sorted({0, 4, 8, 12, ih.HEADER_SIZE, len(self.blob) - 1, len(self.blob) - ih.SIGNATURE_SIZE, *(rng.randrange(len(self.blob)) for _ in range(20))})
        for position in positions:
            changed = bytearray(self.blob)
            changed[position] ^= 1 << rng.randrange(8)
            with self.subTest(position=position), self.assertRaises(ih.ImageError):
                verify(bytes(changed))

    def test_no_prefix_of_a_good_image_is_ever_accepted(self):
        rng = random.Random(7)
        for length in sorted({0, 1, ih.HEADER_SIZE, len(self.blob) - 1, *(rng.randrange(len(self.blob)) for _ in range(10))}):
            with self.subTest(length=length), self.assertRaises(ih.ImageError):
                verify(self.blob[:length])


class SeedFileTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)

    def seed_file(self, text: str) -> str:
        path = self.dir / "seed.txt"
        path.write_text(text, encoding="ascii")
        return str(path)

    def test_a_seed_file_may_end_with_a_newline_and_any_case(self):
        self.assertEqual(ih._read_seed(self.seed_file(SEED.hex().upper() + "\r\n")), SEED)
        self.assertEqual(ih._read_seed(self.seed_file("  " + SEED.hex() + "\n\n")), SEED)

    def test_a_seed_that_is_not_64_hex_digits_is_refused_with_the_file_named(self):
        for text in ("", "zz" * 32, SEED.hex()[:-2], SEED.hex() + "00", "0x" + SEED.hex()):
            path = self.seed_file(text)
            with self.subTest(text[:12]), self.assertRaisesRegex(ih.ImageError, "64 hexadecimal digits"):
                ih._read_seed(path)

    def test_a_missing_seed_file_is_an_os_error(self):
        with self.assertRaises(OSError):
            ih._read_seed(str(self.dir / "absent"))

    def test_versions_are_three_decimal_numbers(self):
        self.assertEqual(ih._parse_version("0.8.12"), (0, 8, 12))
        for bad in ("", "1", "1.2", "1.2.3.4", "1.2.x", "1..3", "-1.2.3", "1.2.3 ", "a.b.c"):
            with self.subTest(bad), self.assertRaisesRegex(ih.ImageError, "major.minor.patch"):
                ih._parse_version(bad)


class ImageHeaderCliTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)
        self.seed = self.dir / "seed"
        self.seed.write_text(SEED.hex() + "\n", encoding="ascii")
        self.image = self.dir / "app.bin"
        self.image.write_bytes(IMAGE)

    def sign(self, *extra, hardware="0x47343031", version="1.2.3"):
        return run(ih, ["sign", "--seed-file", str(self.seed), "--hardware-id", hardware, "--version", version, "--image", str(self.image), *extra])

    def test_keygen_writes_a_new_seed_prints_only_the_public_half_and_never_overwrites(self):
        target = self.dir / "new.seed"
        code, out, err = run(ih, ["keygen", "--out", str(target)])
        self.assertEqual((code, err), (0, ""))
        seed = bytes.fromhex(target.read_text(encoding="ascii").strip())
        self.assertEqual(len(seed), 32)
        self.assertIn(ed25519_ref.public_key(seed).hex(), out)
        self.assertNotIn(seed.hex(), out, "the seed is never printed")
        before = target.read_bytes()
        code, out, err = run(ih, ["keygen", "--out", str(target)])
        self.assertEqual(code, 1)
        self.assertIn("error:", err)
        self.assertEqual(target.read_bytes(), before, "an existing key is never replaced")

    def test_keygen_makes_a_different_seed_every_time(self):
        seeds = set()
        for index in range(3):
            target = self.dir / f"k{index}"
            run(ih, ["keygen", "--out", str(target)])
            seeds.add(target.read_text(encoding="ascii"))
        self.assertEqual(len(seeds), 3)

    def test_pubkey_prints_the_three_forms_and_not_the_seed(self):
        code, out, err = run(ih, ["pubkey", str(self.seed)])
        self.assertEqual((code, err), (0, ""))
        self.assertIn("hex: " + PUBLIC.hex(), out)
        self.assertIn("HYDRA_FW_UPDATE_PUBKEY_HEX=" + PUBLIC.hex(), out)
        self.assertIn("C: " + ih.c_initializer(PUBLIC), out)
        self.assertNotIn(SEED.hex(), out)

    def test_pubkey_of_a_bad_or_missing_file_is_an_error_line_and_exit_1(self):
        bad = self.dir / "bad"
        bad.write_text("not a seed", encoding="ascii")
        for path in (bad, self.dir / "absent"):
            code, out, err = run(ih, ["pubkey", str(path)])
            self.assertEqual((code, out), (1, ""))
            self.assertTrue(err.startswith("error:"))

    def test_sign_prints_a_signature_that_the_verifier_accepts(self):
        code, out, err = self.sign("--link-address", hex(LINK))
        self.assertEqual((code, err), (0, ""))
        signature = bytes.fromhex(out.strip())
        packed = header().pack()
        self.assertEqual(len(signature), 64)
        blob = packed + IMAGE + signature
        self.assertEqual(verify(blob).version, (1, 2, 3))

    def test_sign_can_write_the_signature_to_a_file(self):
        target = self.dir / "app.sig"
        code, out, err = self.sign("--out", str(target))
        self.assertEqual((code, err), (0, ""))
        self.assertIn(str(target), out)
        self.assertEqual(len(target.read_bytes()), 64)

    def test_sign_is_deterministic(self):
        self.assertEqual(self.sign()[1], self.sign()[1])

    def test_sign_uses_the_link_address_of_the_hardware_unless_one_is_given(self):
        code, out, _ = self.sign(hardware="0x47343031")
        self.assertEqual(out.strip(), ih.sign_image(SEED, header(link_address=0x08009000).pack(), IMAGE).hex())
        code, out, _ = self.sign("--link-address", hex(LINK))
        self.assertEqual(out.strip(), ih.sign_image(SEED, header().pack(), IMAGE).hex())

    def test_sign_for_an_unknown_hardware_needs_a_link_address(self):
        code, out, err = self.sign(hardware="0x11223344")
        self.assertEqual((code, out), (1, ""))
        self.assertIn("unknown hardware id", err)
        code, out, err = self.sign("--link-address", "0x08000000", hardware="0x11223344")
        self.assertEqual(code, 0)

    def test_sign_refuses_a_bad_version_an_image_without_vectors_and_a_bad_number(self):
        self.assertEqual(self.sign(version="1.2")[0], 1)
        self.image.write_bytes(b"1234567")
        code, out, err = self.sign()
        self.assertEqual(code, 1)
        self.assertIn("too short", err)
        self.image.write_bytes(IMAGE)
        self.assertEqual(self.sign(hardware="not-a-number")[0], 1)
        self.assertEqual(self.sign(version="70000.0.0")[0], 1, "a version part over 16 bits cannot be packed")

    def test_sign_of_a_missing_image_is_an_error_and_not_a_traceback(self):
        self.image.unlink()
        code, out, err = self.sign()
        self.assertEqual((code, out), (1, ""))
        self.assertTrue(err.startswith("error:"))

    def test_every_known_hardware_id_has_a_link_address_inside_the_flash(self):
        self.assertEqual(set(ih.LINK_ADDRESS_BY_HARDWARE_ID), {0x47343031, 0x48374334, 0x48374337})
        for hardware, link in ih.LINK_ADDRESS_BY_HARDWARE_ID.items():
            self.assertTrue(0x08000000 <= link < 0x08200000, hex(hardware))
            self.assertEqual(link % 0x1000, 0, "linked at a flash sector boundary")


def doc() -> dict:
    return copy.deepcopy(it.load())


class TrustTableTests(unittest.TestCase):
    def setUp(self):
        self.keys = [(PUBLIC, 1), (OTHER_PUBLIC, 2)]

    def test_a_table_round_trips_with_one_or_two_keys(self):
        for keys in ([(PUBLIC, 1)], [(PUBLIC, 2)], self.keys, [(PUBLIC, 4), (OTHER_PUBLIC, 1)], [(PUBLIC, 4)]):
            data = it.pack_table(keys)
            self.assertEqual(len(data), it.table_size(len(keys)))
            self.assertEqual(it.parse_table(data), keys)

    def test_table_sizes_are_56_and_100(self):
        self.assertEqual((it.table_size(1), it.table_size(2)), (56, 100))

    def test_a_table_of_zero_or_three_keys_cannot_be_packed(self):
        for keys in ([], [(PUBLIC, 1), (OTHER_PUBLIC, 2), (bytes(32), 2)]):
            with self.assertRaisesRegex(it.TrustError, "one or two keys"):
                it.pack_table(keys)

    def test_a_key_of_the_wrong_size_or_with_illegal_flags_cannot_be_packed(self):
        for entry in ((PUBLIC[:-1], 1), (PUBLIC + b"\x00", 1), (PUBLIC, 0), (PUBLIC, 8), (PUBLIC, 3), (PUBLIC, 5), (PUBLIC, 6), (PUBLIC, 7), (PUBLIC, 255)):
            with self.subTest(f"{len(entry[0])} bytes, flags {entry[1]}"), self.assertRaisesRegex(it.TrustError, "32 bytes"):
                it.pack_table([entry])

    def test_only_the_flag_sets_current_next_and_revoked_are_legal(self):
        legal = [flags for flags in range(256) if it._flags_ok(flags)]
        self.assertEqual(legal, [1, 2, 4])

    def test_parse_refuses_a_short_buffer_and_a_wrong_magic(self):
        data = it.pack_table([(PUBLIC, 1)])
        for length in range(len(data)):
            with self.subTest(length), self.assertRaises(it.TrustError):
                it.parse_table(data[:length])
        with self.assertRaisesRegex(it.TrustError, "bad magic"):
            it.parse_table(b"XKEY" + data[4:])

    def test_parse_refuses_an_unknown_schema_a_bad_count_and_table_flags(self):
        def rebuilt(schema=1, count=1, table_flags=0):
            body = b"TKEY" + struct.pack("<BBH", schema, count, table_flags) + it.pack_table([(PUBLIC, 1)])[8:-4]
            return body + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)
        for args in ({"schema": 2}, {"schema": 0}, {"count": 0}, {"count": 3}, {"table_flags": 1}):
            with self.subTest(args), self.assertRaisesRegex(it.TrustError, "unknown schema"):
                it.parse_table(rebuilt(**args))

    def test_every_single_bit_flip_of_a_table_is_refused(self):
        data = it.pack_table(self.keys)
        for position in range(len(data)):
            for bit in range(8):
                changed = bytearray(data)
                changed[position] ^= 1 << bit
                with self.assertRaises(it.TrustError, msg=f"byte {position} bit {bit}"):
                    it.parse_table(bytes(changed))

    def test_a_valid_table_with_a_repaired_crc_but_a_bad_entry_is_still_refused(self):
        def resealed(mutate):
            body = bytearray(it.pack_table([(PUBLIC, 1)])[:-4])
            mutate(body)
            return bytes(body) + struct.pack("<I", zlib.crc32(bytes(body)) & 0xFFFFFFFF)
        def wrong_id(b): b[8] ^= 1
        def padding(b): b[8 + 41] = 1
        def zero_flags(b): b[8 + 40] = 0
        def unknown_flag(b): b[8 + 40] = 8
        for name, mutate in {"key id": wrong_id, "padding": padding, "zero flags": zero_flags, "unknown flag": unknown_flag}.items():
            with self.subTest(name), self.assertRaisesRegex(it.TrustError, "key entry is not valid"):
                it.parse_table(resealed(mutate))

    def test_a_table_may_not_list_a_key_twice_or_hold_two_current_keys(self):
        for keys in ([(PUBLIC, 2), (PUBLIC, 4)], [(PUBLIC, 1), (OTHER_PUBLIC, 1)]):
            # packing does not look at the pair, parsing does
            with self.subTest(keys), self.assertRaisesRegex(it.TrustError, "twice|more than one current"):
                it.parse_table(it.pack_table(keys))

    def test_bytes_after_the_table_are_ignored(self):
        data = it.pack_table(self.keys)
        self.assertEqual(it.parse_table(data + b"\xff" * 10), self.keys)

    def test_a_trailing_table_is_cut_short_not_misread(self):
        two = it.pack_table(self.keys)
        header_of_two_with_one_key_size = two[:56]
        with self.assertRaises(it.TrustError):
            it.parse_table(header_of_two_with_one_key_size)

    def test_trusted_keys_skip_the_revoked_and_put_the_current_one_first(self):
        table = [(OTHER_PUBLIC, 2), (PUBLIC, 1)]
        self.assertEqual(it.trusted_keys(table), [PUBLIC, OTHER_PUBLIC])
        self.assertEqual(it.trusted_keys([(PUBLIC, 4), (OTHER_PUBLIC, 2)]), [OTHER_PUBLIC])
        self.assertEqual(it.trusted_keys([(PUBLIC, 4)]), [])
        self.assertEqual(it.trusted_keys([]), [])

    def test_the_closed_table_trusts_nobody_and_is_not_a_parsable_table(self):
        closed = it.closed_table()
        self.assertEqual(closed[:4], b"TKEY")
        self.assertEqual(len(closed), 12)
        self.assertEqual(closed[5], 0, "no keys")
        with self.assertRaises(it.TrustError):
            it.parse_table(closed)
        self.assertEqual(closed[-4:], struct.pack("<I", zlib.crc32(closed[:-4]) & 0xFFFFFFFF))

    def test_the_key_id_is_the_first_eight_bytes_of_the_sha512_of_the_key(self):
        import hashlib
        self.assertEqual(it.key_id(PUBLIC), hashlib.sha512(PUBLIC).digest()[:8])
        self.assertNotEqual(it.key_id(PUBLIC), it.key_id(OTHER_PUBLIC))


class TrustDocumentTests(unittest.TestCase):
    def test_the_published_file_validates_and_builds_both_flavours(self):
        document = doc()
        it.validate(document)
        for flavour in ("dev", "release"):
            data = it.table_bytes(document, flavour)
            self.assertEqual(it.pack_table(it.parse_table(data)), data)

    def test_every_mutation_of_the_header_of_the_document_is_refused(self):
        cases = {
            "format": lambda d: d.update(format=2),
            "schema": lambda d: d.update(schema=2),
            "policy": lambda d: d.update(default_policy="any"),
            "negative counter": lambda d: d.update(min_counter=-1),
            "huge counter": lambda d: d.update(min_counter=1 << 32),
            "text counter": lambda d: d.update(min_counter="0"),
            "offset not hex": lambda d: d["targets"]["rcb"].update(table_offset="1024"),
            "offset not aligned": lambda d: d["targets"]["rcb"].update(table_offset="0x401"),
            "no offset": lambda d: d["targets"]["rcb"].pop("table_offset"),
            "extra flavour": lambda d: d["flavours"].update(beta={"keys": []}),
            "missing flavour": lambda d: d["flavours"].pop("release"),
        }
        for name, mutate in cases.items():
            document = doc()
            mutate(document)
            with self.subTest(name), self.assertRaises(it.TrustError):
                it.validate(document)

    def test_every_mutation_of_the_keys_is_refused(self):
        def entry(public=PUBLIC, state="next"):
            return {"state": state, "public": public.hex(), "key_id": it.key_id(public).hex()}
        cases = {
            "three keys": lambda d: d["flavours"]["release"].update(keys=[entry(PUBLIC), entry(OTHER_PUBLIC), entry(bytes(range(2, 34)))]),
            "two current": lambda d: d["flavours"]["release"].update(keys=[entry(PUBLIC, "current"), entry(OTHER_PUBLIC, "current")]),
            "same key twice": lambda d: d["flavours"]["release"].update(keys=[entry(PUBLIC), entry(PUBLIC)]),
            "short key": lambda d: d["flavours"]["release"].update(keys=[{**entry(), "public": PUBLIC[:-1].hex()}]),
            "unknown state": lambda d: d["flavours"]["release"].update(keys=[entry(state="primary")]),
            "wrong key id": lambda d: d["flavours"]["release"].update(keys=[{**entry(), "key_id": "00" * 8}]),
            "dev without a current key": lambda d: d["flavours"]["dev"].update(keys=[entry()]),
            "dev without keys": lambda d: d["flavours"]["dev"].update(keys=[]),
        }
        for name, mutate in cases.items():
            document = doc()
            mutate(document)
            with self.subTest(name), self.assertRaises((it.TrustError, ValueError)):
                it.validate(document)

    def test_a_release_flavour_without_keys_is_valid_and_builds_the_closed_table(self):
        document = doc()
        document["flavours"]["release"]["keys"] = []
        it.validate(document)
        self.assertEqual(it.bootloader_table(document, "release"), it.closed_table())
        with self.assertRaisesRegex(it.TrustError, "no current key"):
            it.table_bytes(document, "release")

    def test_a_release_flavour_with_only_a_next_key_cannot_build_a_table(self):
        document = doc()
        document["flavours"]["release"]["keys"] = [k for k in document["flavours"]["release"]["keys"] if k["state"] != "current"]
        with self.assertRaisesRegex(it.TrustError, "no current key"):
            it.table_bytes(document, "release")

    def test_the_closed_table_goes_into_the_header_and_says_so(self):
        document = doc()
        document["flavours"]["release"]["keys"] = []
        text = it.header_text(document, "release")
        self.assertIn("closed: no key is trusted", text)
        self.assertIn("#define HYDRA_TRUST_TABLE_SIZE 12u", text)

    def test_the_header_text_lists_every_byte_of_the_table_in_rows_of_twelve(self):
        document = doc()
        text = it.header_text(document, "dev")
        data = it.table_bytes(document, "dev")
        self.assertIn(f"#define HYDRA_TRUST_TABLE_SIZE {len(data)}u", text)
        self.assertIn(f"#define HYDRA_TRUST_MIN_COUNTER {document['min_counter']}UL", text)
        listed = [int(token, 16) for token in text.split("HYDRA_TRUST_TABLE_BYTES", 1)[1].replace("\\", " ").replace(",", " ").split() if token.startswith("0x")]
        self.assertEqual(bytes(listed), data)
        self.assertNotIn("\r", text)

    def test_the_fingerprints_are_the_key_ids_of_the_flavour_in_file_order(self):
        document = doc()
        self.assertEqual(it.fingerprints(document, "release"), [k["key_id"] for k in document["flavours"]["release"]["keys"]])
        document["flavours"]["release"]["keys"] = []
        self.assertEqual(it.fingerprints(document, "release"), [])

    def test_check_binary_tells_a_missing_table_from_a_different_one(self):
        document = doc()
        table = it.table_bytes(document, "dev")
        offset = int(document["targets"]["rcb"]["table_offset"], 16)
        good = bytes(offset) + table + bytes(16)
        self.assertIsNone(it.check_binary(document, good, "rcb", "dev"))
        self.assertIn("no trust table", it.check_binary(document, bytes(offset + 200), "rcb", "dev"))
        self.assertIn("no trust table", it.check_binary(document, b"", "rcb", "dev"), "a binary shorter than the offset has no table")
        changed = bytearray(good)
        changed[offset + 20] ^= 1
        self.assertIn("differs from the dev table", it.check_binary(document, bytes(changed), "rcb", "dev"))
        self.assertIn("differs from the release table", it.check_binary(document, good, "rcb", "release"))
        self.assertIn("0x400", it.check_binary(document, bytes(offset + 200), "rcb", "dev"))


class TrustCliTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)
        self.document = doc()
        patcher = mock.patch.object(it, "load", side_effect=lambda *a, **k: copy.deepcopy(self.document))
        patcher.start()
        self.addCleanup(patcher.stop)

    def fake_root(self) -> Path:
        root = self.dir / "root"
        for directory in it.HEADER_DIRS:
            (root / directory).mkdir(parents=True)
        return root

    def test_validate_passes_on_the_published_file_and_fails_on_a_broken_one(self):
        self.assertEqual(run(it, ["validate"]), (0, "IMAGE_TRUST=PASS\n", ""))
        self.document["default_policy"] = "any"
        code, out, err = run(it, ["validate"])
        self.assertEqual((code, out), (1, ""))
        self.assertIn("IMAGE_TRUST=FAIL", err)

    def test_table_prints_hex_or_a_c_array_of_the_same_bytes(self):
        data = it.table_bytes(self.document, "dev")
        code, out, _ = run(it, ["table", "dev"])
        self.assertEqual((code, out.strip()), (0, data.hex()))
        code, out, _ = run(it, ["table", "dev", "--c"])
        self.assertEqual(bytes(int(t, 16) for t in out.split(", ")), data)

    def test_table_of_a_flavour_without_a_current_key_is_a_failure_line(self):
        self.document["flavours"]["release"]["keys"] = []
        code, out, err = run(it, ["table", "release"])
        self.assertEqual((code, out), (1, ""))
        self.assertIn("no current key", err)

    def test_fingerprints_print_one_key_id_per_line(self):
        code, out, _ = run(it, ["fingerprints", "release"])
        self.assertEqual(out.split(), [k["key_id"] for k in self.document["flavours"]["release"]["keys"]])
        self.document["flavours"]["release"]["keys"] = []
        self.assertEqual(run(it, ["fingerprints", "release"])[1], "\n")

    def test_entry_prints_the_public_entry_for_a_seed_and_never_the_seed(self):
        seed_file = self.dir / "seed"
        seed_file.write_text(SEED.hex(), encoding="ascii")
        code, out, err = run(it, ["entry", str(seed_file)])
        self.assertEqual((code, err), (0, ""))
        printed = json.loads(out)
        self.assertEqual(printed, {"state": "current", "public": PUBLIC.hex(), "key_id": it.key_id(PUBLIC).hex()})
        self.assertNotIn(SEED.hex(), out)
        self.assertEqual(json.loads(run(it, ["entry", str(seed_file), "--state", "next"])[1])["state"], "next")
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            it.main(["entry", str(seed_file), "--state", "revoked"])

    def test_entry_of_a_bad_seed_file_fails_with_a_line_and_no_traceback(self):
        bad = self.dir / "bad"
        bad.write_text("xyz", encoding="ascii")
        code, out, err = run(it, ["entry", str(bad)])
        self.assertEqual((code, out), (1, ""))
        self.assertIn("IMAGE_TRUST=FAIL", err)
        self.assertEqual(run(it, ["entry", str(self.dir / "absent")])[0], 1)

    def test_check_passes_for_the_right_table_and_fails_for_the_wrong_one_or_an_unknown_target(self):
        offset = int(self.document["targets"]["rcb"]["table_offset"], 16)
        binary = self.dir / "boot.bin"
        binary.write_bytes(bytes(offset) + it.table_bytes(self.document, "dev") + bytes(8))
        code, out, _ = run(it, ["check", str(binary), "rcb", "dev"])
        self.assertEqual((code, out), (0, "IMAGE_TRUST=PASS the bootloader holds the dev table\n"))
        code, out, err = run(it, ["check", str(binary), "rcb", "release"])
        self.assertEqual((code, out), (1, ""))
        self.assertIn("differs from the release table", err)
        code, out, err = run(it, ["check", str(binary), "nowhere", "dev"])
        self.assertEqual(code, 1)
        self.assertIn("unknown target nowhere", err)
        self.assertIn("rcb", err, "the known targets are listed")
        binary.write_bytes(bytes(offset + 100))
        self.assertIn("no trust table", run(it, ["check", str(binary), "rcb", "dev"])[2])
        self.assertEqual(run(it, ["check", str(self.dir / "absent.bin"), "rcb", "dev"])[0], 1)

    def test_headers_write_the_three_bootloader_headers_and_then_check_clean(self):
        root = self.fake_root()
        with mock.patch.object(it, "ROOT", root):
            code, out, _ = run(it, ["headers"])
            self.assertEqual((code, out), (0, "IMAGE_TRUST=PASS the generated headers were written\n"))
            for flavour in ("dev", "release"):
                for path in it.header_paths(flavour):
                    self.assertTrue(path.is_file(), str(path))
                    self.assertEqual(path.read_text(encoding="utf-8"), it.header_text(self.document, flavour))
                    self.assertNotIn(b"\r\n", path.read_bytes(), "written with Unix line ends")
            code, out, _ = run(it, ["headers", "--check"])
            self.assertEqual((code, out), (0, "IMAGE_TRUST=PASS the generated headers match\n"))

    def test_headers_check_names_every_missing_or_stale_file(self):
        root = self.fake_root()
        with mock.patch.object(it, "ROOT", root):
            code, out, err = run(it, ["headers", "--check"])
            self.assertEqual((code, out), (1, ""))
            self.assertIn("out of date", err)
            self.assertEqual(err.count("hydra_trust_"), 6, "all six files are listed")
            run(it, ["headers"])
            stale = it.header_paths("release")[1]
            stale.write_text("// edited by hand\n", encoding="utf-8")
            code, out, err = run(it, ["headers", "--check"])
            self.assertEqual(code, 1)
            self.assertEqual(err.count("hydra_trust_"), 1)
            self.assertIn("CM4", err.replace("\\", "/"))

    def test_headers_check_ignores_line_end_differences_only(self):
        root = self.fake_root()
        with mock.patch.object(it, "ROOT", root):
            run(it, ["headers"])
            path = it.header_paths("dev")[0]
            path.write_bytes(path.read_bytes().replace(b"\n", b"\r\n"))
            self.assertEqual(run(it, ["headers", "--check"])[0], 0)

    def test_a_command_is_required_and_an_unknown_flavour_is_a_usage_error(self):
        for argv in ([], ["table", "beta"], ["bogus"]):
            with self.subTest(argv), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as raised:
                it.main(argv)
            self.assertEqual(raised.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
