# =============================================================================
# HYDRA-UMC - spi_bridge protocol tests
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0 - see repo root LICENSE
# =============================================================================
import unittest

from spi_bridge.protocol import SPI_FRAME_SIZE, SPI_TARGET_STACKA, SpiOtaFrame


class SpiOtaFrameTests(unittest.TestCase):
    def test_to_bytes_is_always_exactly_128_bytes(self):
        frame = SpiOtaFrame(SPI_TARGET_STACKA, 3, 0x08, 1, b"\x00")
        self.assertEqual(len(frame.to_bytes()), SPI_FRAME_SIZE)

    def test_round_trip_preserves_every_field(self):
        original = SpiOtaFrame(SPI_TARGET_STACKA, 5, 0x02, 4, b"\xde\xad\xbe\xef")
        restored = SpiOtaFrame.from_bytes(original.to_bytes())
        self.assertEqual(restored.target_tier, original.target_tier)
        self.assertEqual(restored.target_slot, original.target_slot)
        self.assertEqual(restored.frame_type, original.frame_type)
        self.assertEqual(restored.dlc, original.dlc)
        self.assertEqual(restored.payload, original.payload)

    def test_zero_dlc_round_trips_to_empty_payload(self):
        frame = SpiOtaFrame(0, 0, 0x00, 0, b"")
        restored = SpiOtaFrame.from_bytes(frame.to_bytes())
        self.assertEqual(restored.payload, b"")

    def test_from_bytes_rejects_the_wrong_length(self):
        with self.assertRaises(ValueError):
            SpiOtaFrame.from_bytes(b"\x00" * 64)

    def test_payload_over_8_bytes_is_rejected(self):
        with self.assertRaises(ValueError):
            SpiOtaFrame(0, 0, 0x00, 8, b"0123456789")

    def test_dlc_over_8_is_rejected(self):
        with self.assertRaises(ValueError):
            SpiOtaFrame(0, 0, 0x00, 9, b"")

    def test_out_of_range_byte_fields_are_rejected(self):
        for kwargs in ({"target_tier": 256}, {"target_slot": -1}, {"frame_type": 300}):
            with self.subTest(kwargs=kwargs):
                base = {"target_tier": 0, "target_slot": 0, "frame_type": 0, "dlc": 0}
                base.update(kwargs)
                with self.assertRaises(ValueError):
                    SpiOtaFrame(**base)


if __name__ == "__main__":
    unittest.main()
