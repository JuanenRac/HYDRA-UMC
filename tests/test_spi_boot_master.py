# =============================================================================
# HYDRA-UMC - tests/test_spi_boot_master.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The bootloader as SPI master (host model): a signed image comes from the host, and the chain of trust is the one of the CAN update, whole.

Evidence: simulator. The model and these tests are the physical test written down; no board and no Jetson has run them."""
import random
import struct
import sys
import unittest
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

import ed25519_ref  # noqa: E402
import image_header as ih  # noqa: E402
import spi_boot_master as sbm  # noqa: E402

SEED = ih.DEV_SEEDS["H7C7"]
PUBLIC = ed25519_ref.public_key(SEED)
HW = 0x48374337
LINK = 0x08040000
SP = 0x24080000
IMAGE = struct.pack("<II", SP, LINK + 0x101) + bytes(range(256)) * 5  # 1288 bytes: five full pages and a bit more
NONCE = bytes(range(ih.NONCE_SIZE))


def header(version=(0, 1, 8), **over):
    data = dict(hw_id=HW, major=version[0], minor=version[1], patch=version[2], image_size=len(IMAGE), link_address=LINK,
                vector_sp=SP, vector_reset=LINK + 0x101, image_crc32=zlib.crc32(IMAGE) & 0xFFFFFFFF)
    data.update(over)
    return ih.ImageHeader(**data)


def blob(version=(0, 1, 8), seed=SEED, image=IMAGE, **over):
    return ih.build_signed_image(header(version, **over), image, seed)


def config(**over):
    data = dict(spi_master_enabled=True, public_key=PUBLIC)
    data.update(over)
    return sbm.BootConfig(**data)


def run(host, cfg=None, token_wanted=False):
    boot = sbm.SpiMasterBootloader(cfg or config())
    return boot, boot.receive(host, token_wanted)


def host_for(version=(0, 1, 8), **kwargs):
    return sbm.SpiHost(blob(version), version=version, **kwargs)


class SwitchTests(unittest.TestCase):
    def test_the_mode_is_off_by_default_and_the_link_is_not_touched(self):
        self.assertFalse(sbm.BootConfig().spi_master_enabled)
        host = host_for()
        boot, result = run(host, config(spi_master_enabled=False))
        self.assertEqual(result.status, sbm.DISABLED)
        self.assertEqual(host.requests, [])
        self.assertFalse(boot.slot.valid)
        self.assertEqual(result.frames, 0)


class HappyPathTests(unittest.TestCase):
    def test_a_good_image_is_received_verified_and_staged(self):
        host = host_for()
        boot, result = run(host)
        self.assertEqual(result.status, sbm.ACCEPTED, result.reason)
        self.assertTrue(boot.slot.valid)
        self.assertEqual(boot.slot.version, (0, 1, 8))
        self.assertEqual(bytes(boot.slot.data), blob())
        pages = -(-len(blob()) // sbm.MAX_PAGE)
        self.assertEqual(result.pages, pages)
        self.assertEqual(result.frames, 1 + pages + 1)
        kinds = [k for k, _ in host.requests]
        self.assertEqual(kinds[0], sbm.T_HELLO)
        self.assertEqual(kinds[-1], sbm.T_DONE)

    def test_a_host_that_offers_nothing_is_a_refusal_not_an_error(self):
        host = host_for()
        original = host.transaction
        host.transaction = lambda frame: sbm.encode(sbm.T_REFUSE, struct.unpack_from("<H", frame, 2)[0], b"")
        _, result = run(host)
        self.assertEqual(result.status, sbm.REFUSED)
        host.transaction = original


class TrustChainTests(unittest.TestCase):
    def test_a_changed_byte_anywhere_is_refused_and_nothing_stays_valid(self):
        good = blob()
        for position in (0, 8, 20, 63, 64, 70, 500, len(good) - 65, len(good) - 64, len(good) - 1):
            bad = bytearray(good)
            bad[position] ^= 0x01
            boot, result = run(sbm.SpiHost(bytes(bad), version=(0, 1, 8)))
            self.assertNotEqual(result.status, sbm.ACCEPTED, position)
            self.assertFalse(boot.slot.valid, position)
            self.assertEqual(len(boot.slot.data), 0, position)

    def test_an_image_signed_with_another_key_is_refused(self):
        host = sbm.SpiHost(blob(seed=b"x" * 32), version=(0, 1, 8))
        boot, result = run(host)
        self.assertEqual(result.status, sbm.REFUSED)
        self.assertIn("signature", result.reason)
        self.assertFalse(boot.slot.valid)

    def test_the_wrong_hardware_the_wrong_link_and_a_stack_outside_ram_are_refused(self):
        low_stack = struct.pack("<II", 0x20000000, LINK + 0x101) + IMAGE[8:]
        cases = (
            ({"hw_id": 0x47343031}, "hardware"),
            ({"link_address": LINK + 4, "vector_reset": LINK + 0x105}, "address"),
            ({"image": low_stack, "vector_sp": 0x20000000, "image_crc32": zlib.crc32(low_stack) & 0xFFFFFFFF}, "RAM"),
        )
        for over, word in cases:
            host = sbm.SpiHost(blob(**over), version=(0, 1, 8))
            boot, result = run(host)
            self.assertEqual(result.status, sbm.REFUSED, over)
            self.assertIn(word, result.reason, over)
            self.assertFalse(boot.slot.valid)

    def test_anti_rollback_refuses_equal_and_older_versions(self):
        for version in ((0, 1, 7), (0, 1, 6), (0, 0, 9)):
            boot, result = run(sbm.SpiHost(blob(version), version=version))
            self.assertEqual(result.status, sbm.REFUSED, version)
            self.assertIn("anti-rollback", result.reason)
            self.assertFalse(boot.slot.valid)

    def test_a_signed_downgrade_token_for_this_pair_and_this_session_allows_the_older_image(self):
        older = (0, 1, 6)
        image = blob(older)
        digest = ih.image_digest(image[: ih.HEADER_SIZE], image[ih.HEADER_SIZE : -ih.SIGNATURE_SIZE])
        token = ih.downgrade_token(SEED, NONCE, (0, 1, 7), older, digest)
        boot, result = run(sbm.SpiHost(image, version=older, token=token), token_wanted=True)
        self.assertEqual(result.status, sbm.ACCEPTED, result.reason)
        self.assertEqual(boot.slot.version, older)

    def test_a_downgrade_token_for_another_pair_another_nonce_or_another_key_is_refused(self):
        older = (0, 1, 6)
        image = blob(older)
        digest = ih.image_digest(image[: ih.HEADER_SIZE], image[ih.HEADER_SIZE : -ih.SIGNATURE_SIZE])
        tokens = {
            "another installed version": ih.downgrade_token(SEED, NONCE, (0, 1, 9), older, digest),
            "another nonce": ih.downgrade_token(SEED, b"n" * ih.NONCE_SIZE, (0, 1, 7), older, digest),
            "another key": ih.downgrade_token(b"y" * 32, NONCE, (0, 1, 7), older, digest),
            "a short token": b"\x00" * 10,
            "no token": b"",
        }
        for name, token in tokens.items():
            boot, result = run(sbm.SpiHost(image, version=older, token=token), token_wanted=True)
            self.assertEqual(result.status, sbm.REFUSED, name)
            self.assertFalse(boot.slot.valid, name)

    def test_the_token_is_not_asked_for_unless_wanted_so_an_older_image_is_refused_without_it(self):
        older = (0, 1, 6)
        boot, result = run(sbm.SpiHost(blob(older), version=older, token=b"\x01" * 64), token_wanted=False)
        self.assertEqual(result.status, sbm.REFUSED)


class RefusalsBeforeTheTransferTests(unittest.TestCase):
    def pages_asked(self, host):
        return [k for k, _ in host.requests].count(sbm.T_GET_PAGE)

    def test_a_target_that_is_not_this_board_is_refused_before_a_page_is_fetched(self):
        host = host_for(target="G474-CTRL")
        _, result = run(host)
        self.assertEqual(result.status, sbm.REFUSED)
        self.assertEqual(self.pages_asked(host), 0)

    def test_the_target_of_this_board_is_accepted_and_an_empty_one_is_not_checked(self):
        self.assertEqual(run(host_for(target="H757-CM7"))[1].status, sbm.ACCEPTED)
        self.assertEqual(run(host_for(target=None))[1].status, sbm.ACCEPTED)

    def test_a_minimum_bootloader_newer_than_this_one_is_refused_before_a_page_is_fetched(self):
        host = host_for(min_bootloader=(0, 1, 9))
        _, result = run(host)
        self.assertEqual(result.status, sbm.REFUSED)
        self.assertIn("older", result.reason)
        self.assertEqual(self.pages_asked(host), 0)
        self.assertEqual(run(host_for(min_bootloader=(0, 1, 8)))[1].status, sbm.ACCEPTED)
        self.assertEqual(run(host_for(min_bootloader=(0, 0, 1)))[1].status, sbm.ACCEPTED)

    def test_a_size_that_cannot_be_an_image_or_does_not_fit_the_slot_is_refused(self):
        for size in (0, 10, ih.HEADER_SIZE + ih.SIGNATURE_SIZE - 1, sbm.MAX_IMAGE_BYTES + 1, 2**31):
            host = host_for()
            host.oversize_offer = size
            _, result = run(host)
            self.assertEqual(result.status, sbm.REFUSED, size)
            self.assertEqual(self.pages_asked(host), 0, size)


class LinkFaultTests(unittest.TestCase):
    def test_a_damaged_page_is_asked_again(self):
        host = host_for()
        host.corrupt_page, host.corrupt_times = 2, sbm.MAX_RETRIES - 1
        boot, result = run(host)
        self.assertEqual(result.status, sbm.ACCEPTED, result.reason)
        self.assertTrue(boot.slot.valid)

    def test_a_page_that_stays_damaged_ends_the_attempt_with_nothing_staged(self):
        host = host_for()
        host.corrupt_page, host.corrupt_times = 2, 99
        boot, result = run(host)
        self.assertEqual(result.status, sbm.FAILED)
        self.assertFalse(boot.slot.valid)
        self.assertEqual(len(boot.slot.data), 0)

    def test_a_ready_line_that_never_rises_is_a_timeout(self):
        host = host_for()
        host.ready = False
        boot, result = run(host)
        self.assertEqual(result.status, sbm.FAILED)
        self.assertIn("READY", result.reason)

    def test_a_ready_line_that_drops_in_the_middle_is_a_timeout_and_nothing_is_staged(self):
        host = host_for()
        host.drop_ready_after = 3
        boot, result = run(host)
        self.assertEqual(result.status, sbm.FAILED)
        self.assertFalse(boot.slot.valid)

    def test_an_answer_for_another_request_is_never_believed(self):
        host = host_for()
        host.wrong_sequence = True
        _, result = run(host)
        self.assertEqual(result.status, sbm.FAILED)

    def test_an_image_shorter_than_it_offered_fails_and_stages_nothing(self):
        good = blob()
        host = sbm.SpiHost(good[:-100], version=(0, 1, 8))
        host.oversize_offer = len(good)
        boot, result = run(host)
        self.assertEqual(result.status, sbm.FAILED)
        self.assertFalse(boot.slot.valid)

    def test_a_second_attempt_starts_from_an_empty_slot(self):
        boot = sbm.SpiMasterBootloader(config())
        bad = bytearray(blob())
        bad[100] ^= 1
        self.assertEqual(boot.receive(sbm.SpiHost(bytes(bad), version=(0, 1, 8))).status, sbm.REFUSED)
        self.assertEqual(boot.receive(host_for()).status, sbm.ACCEPTED)
        self.assertTrue(boot.slot.valid)
        self.assertEqual(boot.receive(sbm.SpiHost(bytes(bad), version=(0, 1, 8))).status, sbm.REFUSED)
        self.assertFalse(boot.slot.valid)


class FrameTests(unittest.TestCase):
    def test_a_frame_round_trips(self):
        frame = sbm.encode(sbm.T_PAGE, 7, b"abc")
        self.assertEqual(sbm.decode(frame, sbm.HOST_TYPES), (sbm.T_PAGE, 7, b"abc"))
        self.assertEqual(len(frame), sbm.FRAME_OVERHEAD + 3)

    def test_every_damage_to_a_frame_is_refused(self):
        frame = sbm.encode(sbm.T_PAGE, 7, b"abcdef")
        for i in range(len(frame)):
            bad = bytearray(frame)
            bad[i] ^= 0x10
            with self.assertRaises(sbm.FrameError, msg=str(i)):
                sbm.decode(bytes(bad), sbm.HOST_TYPES)
        for short in (b"", frame[:5], frame[:-1], frame + b"\x00"):
            with self.assertRaises(sbm.FrameError):
                sbm.decode(short, sbm.HOST_TYPES)

    def test_a_type_of_the_other_direction_and_an_oversized_payload_are_refused(self):
        with self.assertRaises(sbm.FrameError):
            sbm.decode(sbm.encode(sbm.T_HELLO, 1), sbm.HOST_TYPES)
        with self.assertRaises(sbm.FrameError):
            sbm.encode(sbm.T_PAGE, 1, b"x" * (sbm.MAX_PAYLOAD + 1))
        body = struct.pack("<BBHH", sbm.SYNC, sbm.T_PAGE, 1, 400) + b"x" * 400
        forged = body + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)
        with self.assertRaises(sbm.FrameError):
            sbm.decode(forged, sbm.HOST_TYPES)


class FuzzTests(unittest.TestCase):
    def test_whatever_the_link_does_an_unverified_image_is_never_staged_as_valid(self):
        rng = random.Random(7757)
        good = blob()
        accepted = 0
        for round_number in range(60):
            host = sbm.SpiHost(good, version=(0, 1, 8))
            original = host.transaction

            def damaged(frame, original=original):
                answer = bytearray(original(frame))
                if rng.random() < 0.25:
                    answer[rng.randrange(len(answer))] ^= 1 << rng.randrange(8)
                return bytes(answer)

            host.transaction = damaged
            boot, result = run(host)
            if boot.slot.valid:
                accepted += 1
                self.assertEqual(result.status, sbm.ACCEPTED)
                self.assertEqual(bytes(boot.slot.data), good, round_number)
            else:
                self.assertNotEqual(result.status, sbm.ACCEPTED)
        self.assertGreater(accepted, 0)


if __name__ == "__main__":
    unittest.main()
