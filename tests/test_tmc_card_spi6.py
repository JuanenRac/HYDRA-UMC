# =============================================================================
# HYDRA-UMC - tests/test_tmc_card_spi6.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Source guards of the SPI6 driver of the TMC5160 cards: the pins and the clock are the ones of the profile, the switch ships off and holds the whole unit, the settings come from
config/tmc_card.cfg with the same limits in Python and C, and the driver is built and tested in both runners. Evidence: simulator (the HAL double runs in tests/test_tmc_card_spi6.c)."""
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import platform_profile as pp  # noqa: E402
import tmc_card as tc  # noqa: E402

DRIVER = (ROOT / "src/mcu_stm32h757/CM7/TmcCardSpi6.c").read_text(encoding="utf-8")
HEADER = (ROOT / "src/mcu_stm32h757/CM7/TmcCardSpi6.h").read_text(encoding="utf-8")


class DriverGuards(unittest.TestCase):
    def test_the_switch_ships_off_and_the_whole_unit_is_inside_it(self):
        self.assertIn("#define HYDRA_TMC_CARD_SPI6 0", HEADER)
        self.assertTrue(DRIVER.rstrip().endswith("#endif /* HYDRA_TMC_CARD_SPI6 */"))
        self.assertEqual(DRIVER.count("#if HYDRA_TMC_CARD_SPI6"), 1)

    def test_the_pins_and_the_clock_are_the_ones_of_the_profile(self):
        profile = pp.profile("h757_target")
        spi6 = profile["new_peripherals"]["SPI6"]
        self.assertEqual(spi6["pins"], {"sck": "PG13", "miso": "PG12", "mosi": "PG14", "cs": "PG3"})
        self.assertEqual(spi6["alternate_function"], 5)
        self.assertIn("GPIO_AF5_SPI6", DRIVER)
        self.assertIn("GPIO_PIN_12 | GPIO_PIN_13 | GPIO_PIN_14", DRIVER)
        self.assertIn("cs.Pin = GPIO_PIN_3", DRIVER)
        lines = profile["expansion"]["lines"]
        self.assertEqual((lines["gpio1"], lines["gpio2"]), ("PE5", "PE6"))
        self.assertIn("select.Pin = GPIO_PIN_5 | GPIO_PIN_6", DRIVER)
        self.assertEqual(profile["clocks_hz"]["kernel_spi6"] // 32, spi6["clock_hz"])
        self.assertIn("#define TMC_SPI6_CLOCK_HZ %dUL" % spi6["clock_hz"], DRIVER)
        self.assertIn("SPI_BAUDRATEPRESCALER_32", DRIVER)

    def test_it_never_selects_a_card_on_its_own(self):
        self.assertIn("return TmcCard_Write(&g_ops", DRIVER)
        self.assertIn("return TmcCard_Read(&g_ops", DRIVER)
        self.assertIn("if (!g_ready) return TMC_ERR_TRANSFER;", DRIVER)

    def test_the_driver_is_in_both_runners_and_has_a_floor(self):
        self.assertIn("tests/test_tmc_card_spi6.c", (ROOT / "tools/host_tests.py").read_text(encoding="utf-8"))
        build = (ROOT / "tools/build_firmware.py").read_text(encoding="utf-8")
        self.assertIn("-DHYDRA_TMC_CARD_SPI6=1", build)
        self.assertIn("TmcCardSpi6.c", build)
        floors = json.loads((ROOT / "tools/host_coverage_floor.json").read_text(encoding="utf-8"))
        self.assertEqual(floors["modules"]["src/mcu_stm32h757/CM7/TmcCardSpi6.c"], 100)


class ConfigTests(unittest.TestCase):
    def test_the_file_is_valid(self):
        self.assertEqual(tc.load_config(), {"settle_us": 5, "transfer_timeout_ms": 20})

    def test_values_out_of_range_are_refused(self):
        base = (ROOT / "config/tmc_card.cfg").read_text(encoding="utf-8")
        for field, bad in (("settle_us = 5", "settle_us = 0"), ("settle_us = 5", "settle_us = 1001"), ("transfer_timeout_ms = 20", "transfer_timeout_ms = 0"), ("transfer_timeout_ms = 20", "transfer_timeout_ms = 1001")):
            self.assertIn(field, base)
            with tempfile.TemporaryDirectory() as folder:
                path = Path(folder) / "tmc_card.cfg"
                path.write_text(base.replace(field, bad), encoding="utf-8")
                with self.assertRaises(ValueError, msg=bad):
                    tc.load_config(path)

    def test_the_c_driver_refuses_the_same_limits(self):
        for fragment in ("cfg->settle_us < 1u", "cfg->settle_us > TMC_SPI6_LIMIT_MAX", "cfg->transfer_timeout_ms < 1u", "cfg->transfer_timeout_ms > TMC_SPI6_LIMIT_MAX", "#define TMC_SPI6_LIMIT_MAX 1000u"):
            self.assertIn(fragment, DRIVER)


if __name__ == "__main__":
    unittest.main()
