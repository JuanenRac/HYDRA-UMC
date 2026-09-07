# =============================================================================
# HYDRA-UMC - spi_bridge bootloader_client tests
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0 - see repo root LICENSE
# =============================================================================
import unittest

from fake_transport import FakeBootloaderTransport

from spi_bridge.bootloader_client import SpiOtaFlasher, query_version
from spi_bridge.protocol import SPI_TARGET_STACKA


class QueryVersionTests(unittest.TestCase):
    def test_online_target_reports_hardware_id_and_bootloader_flag(self):
        transport = FakeBootloaderTransport(hardware_id=0x48374334)
        info = query_version(transport, SPI_TARGET_STACKA, 0)
        self.assertTrue(info.online)
        self.assertEqual(info.hardware_id, 0x48374334)
        self.assertFalse(info.is_bootloader)  # fake reports payload[0]=0x00 (application)

    def test_offline_target_is_reported_offline_not_a_crash(self):
        transport = FakeBootloaderTransport(online=False)
        info = query_version(transport, SPI_TARGET_STACKA, 0)
        self.assertFalse(info.online)

    def test_a_transport_failure_is_reported_offline_not_a_crash(self):
        transport = FakeBootloaderTransport()
        transport.raise_on_transceive = OSError("SPI device not found")
        info = query_version(transport, SPI_TARGET_STACKA, 0)
        self.assertFalse(info.online)


class SpiOtaFlasherTests(unittest.TestCase):
    def test_a_full_flash_cycle_ends_in_done(self):
        transport = FakeBootloaderTransport()
        flasher = SpiOtaFlasher(transport, hmac_key=b"\x00" * 32)
        firmware = bytes(range(256)) * 8  # 2048 bytes - exactly one page
        phases = [
            progress.phase
            for progress in flasher.flash(SPI_TARGET_STACKA, 2, 0x48374334, firmware, version_major=0, version_minor=2)
        ]
        self.assertEqual(phases, ["entering_bootloader", "transferring", "verifying", "done"])

    def test_the_real_enter_bootloader_magic_payload_is_sent_first(self):
        transport = FakeBootloaderTransport()
        flasher = SpiOtaFlasher(transport, hmac_key=b"\x00" * 32)
        list(flasher.flash(SPI_TARGET_STACKA, 2, 0x48374334, b"\x00" * 2048, 0, 1))
        first_frame = transport.sent_frames[0]
        self.assertEqual(first_frame.frame_type, 0x00)  # OFS_ENTER_BOOTLOADER
        self.assertEqual(first_frame.payload, bytes((0xB0, 0x07, 0x1D, 0x5A)))

    def test_end_update_carries_the_real_crc32_of_the_firmware(self):
        import zlib

        transport = FakeBootloaderTransport()
        flasher = SpiOtaFlasher(transport, hmac_key=b"\x00" * 32)
        firmware = b"\xAB" * 2048
        list(flasher.flash(SPI_TARGET_STACKA, 2, 0x48374334, firmware, 1, 2))
        end_update_frames = [f for f in transport.sent_frames if f.frame_type == 0x04]  # OFS_END_UPDATE
        self.assertEqual(len(end_update_frames), 1)
        import struct

        sent_crc32, sent_major, sent_minor = struct.unpack(">IHH", end_update_frames[0].payload)
        self.assertEqual(sent_crc32, zlib.crc32(firmware) & 0xFFFFFFFF)
        self.assertEqual((sent_major, sent_minor), (1, 2))

    def test_hmac_is_sent_as_4_real_8_byte_chunks_of_a_real_hmac_sha256(self):
        import hashlib
        import hmac as hmac_module

        key = b"\x11" * 32
        transport = FakeBootloaderTransport()
        flasher = SpiOtaFlasher(transport, hmac_key=key)
        firmware = b"\xCD" * 2048
        list(flasher.flash(SPI_TARGET_STACKA, 2, 0x48374334, firmware, 0, 1))
        hmac_frames = [f for f in transport.sent_frames if f.frame_type == 0x07]  # OFS_HMAC_CHUNK
        self.assertEqual(len(hmac_frames), 4)
        reassembled = b"".join(f.payload for f in hmac_frames)
        expected = hmac_module.new(key, firmware, hashlib.sha256).digest()
        self.assertEqual(reassembled, expected)

    def test_a_page_ack_that_never_arrives_reports_error_not_an_infinite_hang(self):
        transport = FakeBootloaderTransport()
        transport.raise_on_transceive = None

        class NeverAcksTransport(FakeBootloaderTransport):
            def transceive(self, frame):
                if frame.frame_type == 0x03:  # OFS_PAGE_ACK
                    return type(self)._wrong_ack(frame)
                return super().transceive(frame)

            @staticmethod
            def _wrong_ack(frame):
                from spi_bridge.protocol import SpiOtaFrame

                return SpiOtaFrame(frame.target_tier, frame.target_slot, 0x03, 4, b"\xff\xff\xff\xff")

        flasher = SpiOtaFlasher(NeverAcksTransport(), hmac_key=b"\x00" * 32)
        phases = [progress.phase for progress in flasher.flash(SPI_TARGET_STACKA, 2, 0x48374334, b"\x00" * 2048, 0, 1, page_timeout_seconds=0.05)]
        self.assertEqual(phases[-1], "error")


if __name__ == "__main__":
    unittest.main()
