# =============================================================================
# HYDRA-UMC - tests/test_renode_gap_report.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""tools/renode_gap_report.py on a made-up Renode tree: a supported chip lists what it declares, an unsupported one is not invented."""
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

import renode_gap_report as report  # noqa: E402


class GapReportTests(unittest.TestCase):
    def test_a_supported_family_lists_its_peripherals_and_an_absent_one_is_not_supported(self):
        with tempfile.TemporaryDirectory() as tmp:
            cpus = Path(tmp) / "platforms" / "cpus"
            cpus.mkdir(parents=True)
            (cpus / "stm32h743.repl").write_text("spi1: SPI.STM32SPI @ sysbus 0x40013000\nfdcan1: CAN.STM32H7_FDCAN @ sysbus 0x4000A000\n", encoding="utf-8")
            result = report.analyse(report.find_platforms(Path(tmp)))
            self.assertEqual(result["STM32H745"]["files"], ["stm32h743.repl"])
            self.assertEqual(result["STM32H745"]["peripherals"]["SPI"], ["stm32h743.repl"])
            self.assertEqual(result["STM32H745"]["peripherals"]["HSEM"], [])
            self.assertEqual(result["STM32G474"]["files"], [])
            text = report.render(result)
            self.assertIn("NOT SUPPORTED", text)
            self.assertIn("GAP: not declared", text)
            self.assertIn("No firmware ran", text)

    def test_a_directory_without_platforms_is_an_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(report.main([tmp]), 2)


if __name__ == "__main__":
    unittest.main()
