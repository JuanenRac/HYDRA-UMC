# =============================================================================
# HYDRA-UMC - spi_bridge transport tests
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see repo root LICENSE
# =============================================================================
import unittest

from spi_bridge.protocol import SPI_FRAME_SIZE, SpiOtaFrame
from spi_bridge.transport import (
    SEAL_OFFSET_SEQUENCE,
    RealSpiOtaTransport,
    _parse_spidev_path,
    crc16_ccitt_false,
    frame_seal_state,
    open_spi_transport,
    seal_frame,
)


def _spidev_and_gpiod_installed() -> bool:
    try:
        import spidev  # noqa: F401
        import gpiod  # noqa: F401

        return True
    except ImportError:
        return False


class ParseSpidevPathTests(unittest.TestCase):
    def test_real_spidev_path_is_parsed_into_bus_and_device(self):
        self.assertEqual(_parse_spidev_path("/dev/spidev0.0"), (0, 0))
        self.assertEqual(_parse_spidev_path("/dev/spidev1.2"), (1, 2))

    def test_a_malformed_path_is_rejected(self):
        for bad in ("/dev/ttyUSB0", "spidev0", "/dev/spidev0"):
            with self.subTest(bad=bad):
                with self.assertRaises(ValueError):
                    _parse_spidev_path(bad)


class OpenSpiTransportTests(unittest.TestCase):
    def test_missing_spidev_or_gpiod_raises_a_clear_runtime_error_not_an_import_error(self):
        if _spidev_and_gpiod_installed():
            self.skipTest("spidev/gpiod are installed in this environment - nothing to prove here")
        with self.assertRaises(RuntimeError) as context:
            open_spi_transport("/dev/spidev0.0", "/dev/gpiochip0", 3)
        self.assertIn("spidev/gpiod are not installed", str(context.exception))


class _RecordingSpi:
    def __init__(self, reply):
        self._reply = reply
        self.sent = []

    def xfer2(self, request):
        self.sent.append(bytes(request))
        return list(self._reply)

    def close(self):
        pass


class SealedFrameTests(unittest.TestCase):
    def test_the_crc_of_the_check_text_is_the_standard_value(self):
        self.assertEqual(crc16_ccitt_false(b"123456789"), 0x29B1)

    def test_a_sealed_request_matches_the_reference_of_the_repository(self):
        import sys
        from pathlib import Path

        root = Path(__file__).resolve().parents[4]
        sys.path.insert(0, str(root / "tools"))
        import spi_frame  # the reference that also gives the vectors of the firmware test

        frame = SpiOtaFrame(1, 3, 0x08, 0, b"")
        sealed = seal_frame(frame.to_bytes(), 7)
        reference = spi_frame.build(1, 3, 0x08, b"", 7)
        self.assertEqual(sealed, reference)
        self.assertEqual(frame_seal_state(sealed), "ok")

    def test_the_transport_seals_every_request_with_a_new_sequence(self):
        spi = _RecordingSpi(SpiOtaFrame(1, 3, 0x05, 1, b"\x01").to_bytes())
        transport = RealSpiOtaTransport(spi, _Line())
        frame = SpiOtaFrame(1, 3, 0x08, 0, b"")
        transport.transceive(frame)
        transport.transceive(frame)
        self.assertEqual([frame_seal_state(raw) for raw in spi.sent], ["ok", "ok"])
        self.assertEqual([raw[SEAL_OFFSET_SEQUENCE] for raw in spi.sent], [1, 2])
        self.assertEqual(spi.sent[0][:12], frame.to_bytes()[:12])

    def test_the_sequence_wraps_after_255(self):
        spi = _RecordingSpi(SpiOtaFrame(1, 3, 0x05, 1, b"\x01").to_bytes())
        transport = RealSpiOtaTransport(spi, _Line())
        for _ in range(257):
            transport.transceive(SpiOtaFrame(1, 3, 0x08, 0, b""))
        self.assertEqual(spi.sent[255][SEAL_OFFSET_SEQUENCE], 0)
        self.assertEqual(spi.sent[256][SEAL_OFFSET_SEQUENCE], 1)

    def test_sealing_can_be_switched_off(self):
        spi = _RecordingSpi(SpiOtaFrame(1, 3, 0x05, 1, b"\x01").to_bytes())
        transport = RealSpiOtaTransport(spi, _Line(), seal_requests=False)
        transport.transceive(SpiOtaFrame(1, 3, 0x08, 0, b""))
        self.assertEqual(frame_seal_state(spi.sent[0]), "unsealed")

    def test_a_response_with_a_bad_crc_is_a_failed_transfer_and_an_unsealed_one_is_accepted(self):
        good = SpiOtaFrame(1, 3, 0x05, 1, b"\x01").to_bytes()
        damaged = bytearray(seal_frame(good, 9))
        damaged[4] ^= 0x01
        with self.assertRaises(OSError):
            RealSpiOtaTransport(_RecordingSpi(bytes(damaged)), _Line()).transceive(SpiOtaFrame(1, 3, 0x08, 0, b""))
        answer = RealSpiOtaTransport(_RecordingSpi(good), _Line()).transceive(SpiOtaFrame(1, 3, 0x08, 0, b""))
        self.assertEqual(answer.frame_type, 0x05)
        sealed = RealSpiOtaTransport(_RecordingSpi(seal_frame(good, 9)), _Line()).transceive(SpiOtaFrame(1, 3, 0x08, 0, b""))
        self.assertEqual(sealed.frame_type, 0x05)


if __name__ == "__main__":
    unittest.main()


class _FixedReplySpi:
    def __init__(self, reply):
        self._reply = reply

    def xfer2(self, _request):
        return list(self._reply)

    def close(self):
        pass


class _Line:
    def event_wait(self, timeout):
        return True

    def release(self):
        pass


class CorruptReplyTests(unittest.TestCase):
    def test_a_garbage_reply_is_an_oserror_like_any_failed_transfer(self):
        # dlc byte 0xFF is not a frame: from_bytes() raises ValueError, which used
        # to escape through query_version()/flash() (they only expect OSError).
        transport = RealSpiOtaTransport(_FixedReplySpi(bytes([0xFF]) * SPI_FRAME_SIZE), _Line())
        with self.assertRaises(OSError):
            transport.transceive(SpiOtaFrame(0, 0, 0x08, 0))
