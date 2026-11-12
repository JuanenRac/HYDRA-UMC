# =============================================================================
# HYDRA-UMC - spi_bridge: min_bootloader and target of the update manifest
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see repo root LICENSE
# =============================================================================
"""The two optional fields of an update manifest that only the flashing side can enforce: the controller the artifact is built for (`target`) and
the oldest bootloader it can be installed over (`min_bootloader`). A manifest that is wrong on either is refused before a single byte of the image
goes out; a manifest without them changes nothing. The board is the in-memory fake transport."""

import json
import struct
import threading
import unittest
from urllib.error import HTTPError
from urllib.request import Request, urlopen

from fake_transport import FakeBootloaderTransport

import spi_bridge.bootloader_client as client
from spi_bridge.bootloader_client import SpiOtaFlasher, check_manifest_requirements, parse_version, query_bootloader_version
from spi_bridge.http_service import serve
from spi_bridge.protocol import OFS_DATA, OFS_ENTER_BOOTLOADER, SPI_TARGET_STACKA

H7C4 = 0x48374334
G401 = 0x47343031
URTC_HEAD = 0x55544331  # not a board of the HYDRA-UMC bootloaders
SIGNATURE = bytes(64)
IMAGE = struct.pack("<II", 0x20010000, 0x08140101) + bytes(range(200))


def run(transport, hardware_id=H7C4, **options):
    flasher = SpiOtaFlasher(transport, b"\x00" * 32)
    original = client.time.sleep
    client.time.sleep = lambda _seconds: None
    try:
        return list(flasher.flash(SPI_TARGET_STACKA, 0, hardware_id, IMAGE, 0, 3, version_patch=1, signature=SIGNATURE, **options))
    finally:
        client.time.sleep = original


def sent_types(transport):
    return [frame.frame_type for frame in transport.sent_frames]


class VersionParsingTests(unittest.TestCase):
    def test_three_and_four_numbers_are_versions(self):
        self.assertEqual(parse_version("0.2.0"), (0, 2, 0))
        self.assertEqual(parse_version("0.8.0.1"), (0, 8, 0, 1))

    def test_anything_else_is_refused(self):
        for text in ("", "1", "1.2", "1.2.x", "1.2.3.4.5", "1..3", "-1.2.3", "1.2.3 ", "١.2.3"):
            with self.assertRaises(ValueError, msg=text):
                parse_version(text)


class RequirementsBeforeTheBoardTests(unittest.TestCase):
    def test_no_fields_ask_for_nothing(self):
        self.assertIsNone(check_manifest_requirements(H7C4, None, None))
        self.assertIsNone(check_manifest_requirements(URTC_HEAD, None, None))

    def test_the_target_of_each_known_board(self):
        for hardware_id, name in ((G401, "g474"), (H7C4, "h745-cm4"), (0x48374337, "h745-cm7")):
            self.assertIsNone(check_manifest_requirements(hardware_id, None, name))
            self.assertIn("target", check_manifest_requirements(hardware_id, None, "other"))

    def test_a_target_for_a_board_that_is_not_known_is_refused(self):
        self.assertIn("0x55544331", check_manifest_requirements(URTC_HEAD, None, "g474"))

    def test_a_minimum_that_is_not_a_version_is_refused(self):
        self.assertIn("MAJOR.MINOR.PATCH", check_manifest_requirements(H7C4, "1.x", None))


class QueryBootloaderVersionTests(unittest.TestCase):
    def test_the_version_the_bootloader_sends(self):
        self.assertEqual(query_bootloader_version(FakeBootloaderTransport(bootloader_version=(0, 1, 9)), SPI_TARGET_STACKA, 0), (0, 1, 9))

    def test_an_application_or_a_silent_board_gives_none(self):
        self.assertIsNone(query_bootloader_version(FakeBootloaderTransport(), SPI_TARGET_STACKA, 0))
        self.assertIsNone(query_bootloader_version(FakeBootloaderTransport(online=False), SPI_TARGET_STACKA, 0))

    def test_a_transport_failure_gives_none(self):
        transport = FakeBootloaderTransport(bootloader_version=(0, 1, 9))
        transport.raise_on_transceive = OSError("SPI device not found")
        self.assertIsNone(query_bootloader_version(transport, SPI_TARGET_STACKA, 0))


class FlashRefusalTests(unittest.TestCase):
    def test_a_manifest_for_another_target_never_touches_the_board(self):
        transport = FakeBootloaderTransport(hardware_id=H7C4, bootloader_version=(0, 1, 9))
        progress = run(transport, target="g474")
        self.assertEqual([step.phase for step in progress], ["error"])
        self.assertIn("target", progress[-1].error)
        self.assertEqual(transport.sent_frames, [])

    def test_a_bootloader_that_is_too_old_is_refused_before_any_image_byte(self):
        transport = FakeBootloaderTransport(bootloader_version=(0, 1, 9))
        progress = run(transport, min_bootloader="0.2.0")
        self.assertEqual(progress[-1].phase, "error")
        self.assertIn("at least 0.2.0", progress[-1].error)
        self.assertIn("0.1.9", progress[-1].error)
        self.assertNotIn(OFS_DATA, sent_types(transport))
        self.assertEqual(transport.pages_received, 0)

    def test_the_minimum_itself_and_anything_newer_are_accepted(self):
        for installed in ((0, 2, 0), (0, 2, 1), (1, 0, 0)):
            transport = FakeBootloaderTransport(bootloader_version=installed)
            progress = run(transport, min_bootloader="0.2.0", target="h745-cm4")
            self.assertEqual(progress[-1].phase, "done", installed)

    def test_four_numbers_compare_with_a_missing_fourth_as_zero(self):
        self.assertEqual(run(FakeBootloaderTransport(bootloader_version=(0, 8, 0)), min_bootloader="0.8.0.0")[-1].phase, "done")
        self.assertEqual(run(FakeBootloaderTransport(bootloader_version=(0, 8, 0)), min_bootloader="0.8.0.1")[-1].phase, "error")

    def test_a_bootloader_whose_version_cannot_be_read_is_refused_not_assumed(self):
        transport = FakeBootloaderTransport()  # never sends the frame of the version
        progress = run(transport, min_bootloader="0.1.0")
        self.assertEqual(progress[-1].phase, "error")
        self.assertIn("could not be read", progress[-1].error)
        self.assertNotIn(OFS_DATA, sent_types(transport))

    def test_the_board_is_asked_into_its_bootloader_before_the_version_is_read(self):
        transport = FakeBootloaderTransport(bootloader_version=(0, 1, 9))
        run(transport, min_bootloader="0.2.0")
        self.assertEqual(sent_types(transport)[0], OFS_ENTER_BOOTLOADER)

    def test_without_the_fields_the_flash_is_as_before(self):
        transport = FakeBootloaderTransport()
        progress = run(transport)
        self.assertEqual(progress[-1].phase, "done")
        self.assertNotIn(0x0A, [frame.payload[0] for frame in transport.sent_frames if frame.frame_type == 0x17])


class HttpManifestRequirementsTests(unittest.TestCase):
    def setUp(self):
        self.transport = FakeBootloaderTransport(hardware_id=H7C4, bootloader_version=(0, 1, 9))
        self.server = serve(self.transport, hmac_key=b"\x00" * 32, port=0)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.url = f"http://127.0.0.1:{self.server.server_port}/flash?tier=2&slot=0&hardware_id={H7C4}&version_major=0&version_minor=3&signature={'ab' * 64}"
        original = client.time.sleep
        client.time.sleep = lambda _seconds: None
        self.addCleanup(setattr, client.time, "sleep", original)

    def tearDown(self):
        self.server.shutdown()
        self.thread.join()
        self.server.server_close()

    def post(self, query=""):
        return urlopen(Request(self.url + query, data=IMAGE, method="POST"), timeout=10)

    def test_a_wrong_target_is_a_400_before_the_body_is_read(self):
        with self.assertRaises(HTTPError) as caught:
            self.post("&target=g474")
        self.assertEqual(caught.exception.code, 400)
        self.assertIn("target", json.loads(caught.exception.read())["error"])
        self.assertEqual(self.transport.sent_frames, [])

    def test_a_minimum_that_is_not_a_version_is_a_400(self):
        with self.assertRaises(HTTPError) as caught:
            self.post("&min_bootloader=new")
        self.assertEqual(caught.exception.code, 400)

    def test_a_bootloader_too_old_ends_the_stream_with_an_error(self):
        lines = [json.loads(line) for line in self.post("&min_bootloader=0.2.0&target=h745-cm4").read().decode().splitlines()]
        self.assertEqual(lines[-1]["phase"], "error")
        self.assertIn("at least 0.2.0", lines[-1]["error"])
        self.assertNotIn("transferring", [line["phase"] for line in lines])

    def test_a_manifest_the_board_satisfies_is_flashed(self):
        lines = [json.loads(line) for line in self.post("&min_bootloader=0.1.9&target=h745-cm4").read().decode().splitlines()]
        self.assertEqual(lines[-1]["phase"], "done")


if __name__ == "__main__":
    unittest.main()
