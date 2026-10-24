# =============================================================================
# HYDRA-UMC - spi_bridge: the signed update, from the CM5 to a bootloader model
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see repo root LICENSE
# =============================================================================
"""What SpiOtaFlasher sends to a HYDRA-UMC bootloader (a signed 64-byte header, a tag over header || image, and for a downgrade a token built
from the nonce the board hands out) is checked against tools/image_header.py, the host reference of the repository whose vectors the C code of
the three bootloaders is tested against. The bootloader here is a model that reassembles the frames the way the C does and applies the
reference's own verification: so an update that this client builds and the model accepts is one the reference accepts."""
import struct
import sys
import unittest
import zlib
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parents[4]
TOOLS = REPOSITORY / "tools"
if TOOLS.is_dir():
    sys.path.insert(0, str(TOOLS))
    import image_header as reference  # noqa: E402
else:  # the package deployed on its own: nothing to compare with
    reference = None

from spi_bridge import image_header as client_header
from spi_bridge.bootloader_client import SpiOtaFlasher
from spi_bridge.protocol import (
    OFS_DATA,
    OFS_DOWNGRADE_NONCE_REQUEST,
    OFS_DOWNGRADE_NONCE_RESPONSE,
    OFS_DOWNGRADE_TOKEN,
    OFS_END_UPDATE,
    OFS_HMAC_CHUNK,
    OFS_IMAGE_HEADER,
    OFS_PAGE_ACK,
    OFS_POLL_STATUS,
    OFS_START_UPDATE,
    OFS_STATUS,
    SPI_TARGET_STACKA,
    STATUS_VERIFY_FAIL,
    STATUS_VERIFY_OK,
    VERIFY_FAIL_REASON_ROLLBACK,
    SpiOtaFrame,
)

KEY = bytes(range(0x40, 0x60))
G401 = 0x47343031
H7C4 = 0x48374334
RAM = (0x20000100, 0x20018000)


def firmware(size: int = 3000, seed: int = 7, link: int = 0x08009000) -> bytes:
    body = bytes((i * seed + 3) & 0xFF for i in range(size - 8))
    return struct.pack("<II", 0x20010000, link + 0x101) + body


class BootloaderModel:
    """The frames of the C bootloader's update, reassembled, and its decision made by the reference (tools/image_header.py)."""

    def __init__(self, hardware_id=G401, installed=(0, 2, 2), key=KEY):
        self.hardware_id = hardware_id
        self.installed = installed
        self.key = key
        self.nonce = bytes((0x30 + i * 5) & 0xFF for i in range(16))
        self.sent = []
        self.reset()
        self.pending_nonce_half = None

    def reset(self):
        self.image = bytearray()
        self.tag = b""
        self.header = b""
        self.token = b""
        self.size = 0
        self.verdict = None
        self.pages = 0

    def transceive(self, frame):
        self.sent.append(frame)
        kind = frame.frame_type
        payload = bytes(frame.payload[: frame.dlc])
        if kind == OFS_POLL_STATUS:
            kind, payload = payload[0], b""  # a poll waits for the next frame of that type
        if kind == OFS_START_UPDATE:
            self.reset()
            self.size = struct.unpack(">I", payload[:4])[0]
        elif kind == OFS_HMAC_CHUNK:
            self.tag += payload
        elif kind == OFS_IMAGE_HEADER:
            self.header += payload
        elif kind == OFS_DOWNGRADE_TOKEN:
            self.token += payload
        elif kind == OFS_DOWNGRADE_NONCE_REQUEST:
            self.pending_nonce_half = payload[0]
        elif kind == OFS_DATA:
            self.image += payload
            if len(self.image) % 2048 == 0 or len(self.image) == self.size:
                self.pages += 1
        elif kind == OFS_END_UPDATE:
            self.verdict = self.decide(struct.unpack(">IHH", payload))
        elif kind == OFS_DOWNGRADE_NONCE_RESPONSE and self.pending_nonce_half is not None:
            half, self.pending_nonce_half = self.pending_nonce_half, None
            return SpiOtaFrame(frame.target_tier, frame.target_slot, OFS_DOWNGRADE_NONCE_RESPONSE, 8, self.nonce[8 * half : 8 * half + 8])
        elif kind == OFS_PAGE_ACK:
            return SpiOtaFrame(frame.target_tier, frame.target_slot, OFS_PAGE_ACK, 4, struct.pack(">I", max(0, self.pages - 1)))
        elif kind == OFS_STATUS and self.verdict is not None:
            if self.verdict == 0:
                return SpiOtaFrame(frame.target_tier, frame.target_slot, OFS_STATUS, 1, bytes([STATUS_VERIFY_OK]))
            return SpiOtaFrame(frame.target_tier, frame.target_slot, OFS_STATUS, 2, bytes([STATUS_VERIFY_FAIL, self.verdict]))
        return SpiOtaFrame(frame.target_tier, frame.target_slot, 0xFF, 0)

    def decide(self, end):
        crc, major, minor = end
        if len(self.image) != self.size or len(self.tag) != 32 or len(self.header) != 64:
            return 0x01
        if crc != zlib.crc32(bytes(self.image)) & 0xFFFFFFFF:
            return 0x02
        blob = bytes(self.header) + bytes(self.image) + bytes(self.tag)
        link = client_header.LINK_ADDRESS_BY_HARDWARE_ID[self.hardware_id]
        try:
            parsed = reference.verify_image(
                blob, self.key, hw_id=self.hardware_id, link_address=link, installed_version=self.installed, ram_range=RAM,
                nonce=self.nonce, token=self.token if len(self.token) == 32 else b"",
            )
        except reference.ImageError as error:
            text = str(error)
            if "tag" in text:
                return 0x03
            if "anti-rollback" in text:
                return VERIFY_FAIL_REASON_ROLLBACK
            if "hardware" in text:
                return 0x04
            return 0x06
        return 0 if (parsed.major, parsed.minor) == (major, minor) else 0x06


def flash(model, image, version, **options):
    flasher = SpiOtaFlasher(model, KEY)
    import spi_bridge.bootloader_client as module

    original = module.time.sleep
    module.time.sleep = lambda _seconds: None
    try:
        return list(flasher.flash(SPI_TARGET_STACKA, 0, model.hardware_id, image, version[0], version[1], version_patch=version[2], **options))
    finally:
        module.time.sleep = original


@unittest.skipIf(reference is None, "tools/image_header.py (the host reference) is not next to this package")
class SignedFlashTests(unittest.TestCase):
    def test_what_the_client_builds_is_what_the_reference_builds(self):
        image = firmware()
        header = client_header.build_header(G401, (0, 2, 3), image)
        stack, reset = struct.unpack_from("<II", image, 0)
        expected = reference.ImageHeader(
            hw_id=G401, major=0, minor=2, patch=3, image_size=len(image), link_address=0x08009000, vector_sp=stack, vector_reset=reset,
            image_crc32=zlib.crc32(image) & 0xFFFFFFFF,
        )
        self.assertEqual(header, expected.pack())
        tag = client_header.tag_of(KEY, header, image)
        self.assertEqual(tag, reference.build_signed_image(expected, image, KEY)[-32:])
        nonce = bytes(range(16))
        self.assertEqual(
            client_header.downgrade_token(KEY, nonce, (0, 2, 2), (0, 1, 8), tag),
            reference.downgrade_token(KEY, nonce, (0, 2, 2), (0, 1, 8), tag),
        )

    def test_the_link_addresses_are_the_ones_of_the_three_bootloaders(self):
        sources = {
            0x47343031: REPOSITORY / "src/mcu_stm32g474/boot/bootloader_common.h",
            0x48374334: REPOSITORY / "src/mcu_stm32h745/CM4/boot/bootloader_common.h",
            0x48374337: REPOSITORY / "src/mcu_stm32h745/CM7/boot/bootloader_common.h",
        }
        import re

        for hardware_id, path in sources.items():
            text = path.read_text(encoding="utf-8")
            main = int(re.search(r"#define MAIN_APP_ADDR\s+(0x[0-9A-Fa-f]+)", text).group(1), 16)
            hw = int(re.search(r"#define THIS_HARDWARE_ID\s+(0x[0-9A-Fa-f]+)", text).group(1), 16)
            self.assertEqual(hw, hardware_id, path.name)
            self.assertEqual(client_header.LINK_ADDRESS_BY_HARDWARE_ID[hardware_id], main, path.name)

    def test_a_newer_image_is_accepted_by_the_reference(self):
        model = BootloaderModel()
        progress = flash(model, firmware(), (0, 2, 3))
        self.assertEqual(progress[-1].phase, "done", progress[-1])
        self.assertEqual(len([f for f in model.sent if f.frame_type == OFS_IMAGE_HEADER]), 8)
        self.assertEqual(len([f for f in model.sent if f.frame_type == OFS_HMAC_CHUNK]), 4)

    def test_the_patch_decides_the_rollback(self):
        model = BootloaderModel(installed=(0, 1, 8))
        self.assertEqual(flash(model, firmware(), (0, 1, 2))[-1].phase, "error")
        self.assertEqual(model.verdict, VERIFY_FAIL_REASON_ROLLBACK)
        model = BootloaderModel(installed=(0, 1, 8))
        self.assertEqual(flash(model, firmware(), (0, 1, 9))[-1].phase, "done")

    def test_a_downgrade_needs_the_installed_version_and_then_the_board_accepts_the_token(self):
        model = BootloaderModel(installed=(0, 2, 2))
        refused = flash(model, firmware(), (0, 1, 8), allow_downgrade=True)
        self.assertEqual(refused[-1].phase, "error")
        self.assertIn("downgrade_from", refused[-1].error)
        self.assertFalse(model.sent, "nothing was sent to the board")

        model = BootloaderModel(installed=(0, 2, 2))
        accepted = flash(model, firmware(), (0, 1, 8), allow_downgrade=True, downgrade_from=(0, 2, 2))
        self.assertEqual(accepted[-1].phase, "done", accepted[-1])
        requests = [f for f in model.sent if f.frame_type == OFS_DOWNGRADE_NONCE_REQUEST]
        self.assertEqual([bytes(f.payload[:1]) for f in requests], [b"\x00", b"\x01"])
        self.assertEqual(len([f for f in model.sent if f.frame_type == OFS_DOWNGRADE_TOKEN]), 4)
        self.assertFalse([f for f in model.sent if f.frame_type == 0x0D], "the old constant frame is never sent to a HYDRA-UMC board")

    def test_a_token_built_for_the_wrong_installed_version_is_refused_by_the_board(self):
        model = BootloaderModel(installed=(0, 2, 2))
        result = flash(model, firmware(), (0, 1, 8), downgrade_from=(0, 2, 5))
        self.assertEqual(result[-1].phase, "error")
        self.assertEqual(model.verdict, VERIFY_FAIL_REASON_ROLLBACK)

    def test_a_board_that_does_not_give_its_nonce_stops_the_update(self):
        class Silent(BootloaderModel):
            def transceive(self, frame):
                if frame.frame_type == OFS_DOWNGRADE_NONCE_RESPONSE:
                    return SpiOtaFrame(frame.target_tier, frame.target_slot, 0xFF, 0)
                return super().transceive(frame)

        result = flash(Silent(installed=(0, 2, 2)), firmware(), (0, 1, 8), downgrade_from=(0, 2, 2))
        self.assertEqual(result[-1].phase, "error")
        self.assertIn("nonce", result[-1].error)

    def test_the_other_cores_get_their_own_link_address(self):
        for hardware_id, link in ((H7C4, 0x08140000), (0x48374337, 0x08040000)):
            model = BootloaderModel(hardware_id=hardware_id)
            progress = flash(model, firmware(link=link), (0, 2, 3))
            self.assertEqual(progress[-1].phase, "done", (hex(hardware_id), progress[-1]))

    def test_a_wrong_key_is_a_tag_failure(self):
        model = BootloaderModel(key=b"\x01" * 32)
        self.assertEqual(flash(model, firmware(), (0, 2, 3))[-1].phase, "error")
        self.assertEqual(model.verdict, 0x03)

    def test_a_version_that_does_not_fit_is_refused_before_anything_is_sent(self):
        model = BootloaderModel()
        result = flash(model, firmware(), (0, 70000, 0))
        self.assertEqual(result[-1].phase, "error")
        self.assertFalse(model.sent)

    def test_an_urtc_board_behind_the_relay_still_gets_the_old_image_format_and_frame(self):
        class Legacy(BootloaderModel):
            def __init__(self):
                super().__init__()
                self.hardware_id = 0x0303CC01

        model = Legacy()
        flasher = SpiOtaFlasher(model, KEY)
        import spi_bridge.bootloader_client as module

        original = module.time.sleep
        module.time.sleep = lambda _seconds: None
        try:
            list(flasher.flash(SPI_TARGET_STACKA, 3, 0x0303CC01, firmware(), 0, 1, allow_downgrade=True))
        finally:
            module.time.sleep = original
        self.assertTrue([f for f in model.sent if f.frame_type == 0x0D], "the URTC bootloader still understands the magic frame")
        self.assertFalse([f for f in model.sent if f.frame_type == OFS_IMAGE_HEADER], "and takes no header")


class HeaderBuilderTests(unittest.TestCase):
    def test_only_the_hydra_bootloaders_take_a_signed_header(self):
        for hardware_id in (G401, H7C4, 0x48374337):
            self.assertTrue(client_header.is_signed_header_target(hardware_id))
        self.assertFalse(client_header.is_signed_header_target(0x0303CC01))
        with self.assertRaises(client_header.ImageHeaderError):
            client_header.build_header(0x0303CC01, (0, 1, 0), firmware())

    def test_an_image_without_a_vector_table_is_refused(self):
        with self.assertRaises(client_header.ImageHeaderError):
            client_header.build_header(G401, (0, 1, 0), b"\x00" * 4)

    def test_nonce_and_tag_sizes_are_checked(self):
        with self.assertRaises(client_header.ImageHeaderError):
            client_header.downgrade_token(KEY, b"short", (0, 1, 0), (0, 0, 9), b"\x00" * 32)
        with self.assertRaises(client_header.ImageHeaderError):
            client_header.downgrade_token(KEY, b"\x00" * 16, (0, 1, 0), (0, 0, 9), b"\x00" * 31)


if __name__ == "__main__":
    unittest.main()
