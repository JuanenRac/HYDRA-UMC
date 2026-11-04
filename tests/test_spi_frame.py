# =============================================================================
# HYDRA-UMC - tests/test_spi_frame.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The sealed SPI frame: the Python reference, the vectors, the C test and the two copies of the header say the same thing."""
import json
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import spi_frame  # noqa: E402

VECTORS = json.loads((ROOT / "tests" / "spi_frame_vectors.json").read_text(encoding="utf-8"))
HEADER_FIRMWARE = ROOT / "src/mcu_stm32h745/CM4/boot/spi_frame_crc.h"
HEADER_HOST = ROOT / "src/cm5_host/ipc_driver/src/spi_frame_crc.h"


class SpiFrameTests(unittest.TestCase):
    def test_the_standard_check_value(self):
        self.assertEqual(spi_frame.crc16(VECTORS["crc16_ccitt_false_check"]["input_ascii"].encode()), int(VECTORS["crc16_ccitt_false_check"]["crc"], 16))

    def test_the_vectors_are_what_the_reference_builds(self):
        for vector in VECTORS["frames"]:
            head = bytes.fromhex(vector["wire_hex"])
            wire = spi_frame.build(head[0], head[1], head[2], head[4:4 + head[3]], head[13])
            self.assertEqual(wire[:16], head, vector["name"])
            self.assertEqual(int(vector["crc"], 16), head[14] << 8 | head[15], vector["name"])
            self.assertEqual(spi_frame.check(wire), "ok", vector["name"])

    def test_the_c_test_carries_the_same_vectors(self):
        source = (ROOT / "tests" / "test_spi_frame_crc.c").read_text(encoding="utf-8")
        found = re.findall(r'check_vector\("(\w+)", "([0-9a-f]{32})", (0x[0-9a-f]+)\)', source)
        self.assertEqual(found, [(v["name"], v["wire_hex"], v["crc"]) for v in VECTORS["frames"]])

    def test_a_flipped_bit_is_caught_and_a_retry_is_a_duplicate(self):
        wire = spi_frame.build(2, 1, 0x10, b"\x01", 5)
        self.assertEqual(spi_frame.judge(wire, None), "dispatch")
        self.assertEqual(spi_frame.judge(wire, 5), "duplicate")
        self.assertEqual(spi_frame.judge(wire, 4), "dispatch")
        for bit in range(8 * spi_frame.CRC_SPAN):
            damaged = bytearray(wire)
            damaged[bit // 8] ^= 1 << (bit % 8)
            self.assertEqual(spi_frame.check(bytes(damaged)), "bad_crc" if damaged[spi_frame.OFS_MARKER] == spi_frame.MARKER else "unsealed")
        self.assertEqual(spi_frame.check(b"\xff" * 128), "unsealed")

    def test_the_two_copies_of_the_header_are_the_same_file(self):
        self.assertEqual(HEADER_FIRMWARE.read_bytes().replace(b"\r\n", b"\n"), HEADER_HOST.read_bytes().replace(b"\r\n", b"\n"))


if __name__ == "__main__":
    unittest.main()
