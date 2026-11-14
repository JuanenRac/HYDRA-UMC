# =============================================================================
# HYDRA-UMC - tests/test_host_link_spi3_driver.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Source guards of the SPI3 driver of the host link (the HAL cannot run on the host; the compile check is a step of tools/build_firmware.py, and the logic it drives is tested through the
transport). Evidence: simulator."""
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import platform_profile as pp  # noqa: E402

DRIVER = (ROOT / "src/mcu_stm32h757/CM7/HostLinkSpi3.c").read_text(encoding="utf-8")
HEADER = (ROOT / "src/mcu_stm32h757/CM7/HostLinkSpi3.h").read_text(encoding="utf-8")
SCRIPT = (ROOT / "tools" / "build_firmware.py").read_text(encoding="utf-8").replace("\r\n", "\n")


class HostLinkSpi3DriverTests(unittest.TestCase):
    def test_the_switch_ships_off_and_the_whole_unit_is_inside_it(self):
        self.assertIn("#define HYDRA_HOST_LINK_MASTER 0", HEADER)
        self.assertTrue(DRIVER.rstrip().endswith("#endif /* HYDRA_HOST_LINK_MASTER */"))
        self.assertEqual(DRIVER.count("#if HYDRA_HOST_LINK_MASTER"), 1)

    def test_the_pins_are_the_ones_of_the_profile(self):
        pins = pp.profile("h757_target")["host_spi"]["pins"]
        self.assertEqual(pins, {"sck": "PB3", "miso": "PB4", "mosi": "PB5", "nss": "PA15"})
        self.assertIn("clk_and_miso.Pin = GPIO_PIN_3 | GPIO_PIN_4", DRIVER)
        self.assertIn("mosi.Pin = GPIO_PIN_5", DRIVER)
        self.assertIn("nss.Pin = GPIO_PIN_15", DRIVER)
        self.assertEqual(pp.profile("h757_target")["host_spi"]["alternate_function"], {"sck": 6, "miso": 6, "mosi": 7, "nss": 6})
        self.assertIn("GPIO_AF6_SPI3", DRIVER)
        self.assertIn("GPIO_AF7_SPI3", DRIVER)

    def test_the_clock_is_checked_against_the_profile_when_it_compiles(self):
        self.assertIn("HYDRA_CLOCK_KERNEL_SPI3_HZ / HYDRA_HOST_SPI_PRESCALER", DRIVER)
        host = pp.profile("h757_target")["host_spi"]
        self.assertEqual(pp.profile("h757_target")["clocks_hz"]["kernel_spi3"] // host["prescaler"], host["clock_hz"])
        for value in (2, 4, 8, 16, 32, 64, 128, 256):
            self.assertIn(f"HYDRA_HOST_SPI_PRESCALER == {value}", DRIVER)

    def test_a_wired_ready_line_without_a_pin_is_refused_and_a_failed_transfer_is_aborted(self):
        self.assertIn("if (cfg->ready_known) return 0;", DRIVER)
        self.assertIn("HAL_SPI_Abort", DRIVER)
        self.assertIn("HAL_GPIO_WritePin(GPIOA, GPIO_PIN_15, GPIO_PIN_SET);", DRIVER)

    def test_the_build_compiles_it_for_the_h757_platform_and_never_links_it(self):
        self.assertTrue(re.search(r"\"-DHYDRA_PLATFORM_H757\", \"-DHYDRA_HOST_LINK_MASTER=1\".*HostLinkSpi3\.c", SCRIPT, re.DOTALL))


if __name__ == "__main__":
    unittest.main()
