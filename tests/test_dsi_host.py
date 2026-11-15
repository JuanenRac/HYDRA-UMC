# =============================================================================
# HYDRA-UMC - tests/test_dsi_host.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The DSI host of the CM7: the mapping of the plan to the HAL values is complete (every input divider, output divider, colour coding and lane count the plan can give has a HAL
macro), the escape clock stays under 20 MHz for every plan the sweep makes, the switch ships off, and the new source is in both runners with a floor. Evidence: simulator
(the host double runs in tests/test_dsi_host.c)."""
import json
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import dsi_plan as dp  # noqa: E402

HOST_C = (ROOT / "src/mcu_stm32h757/CM7/DsiHost.c").read_text(encoding="utf-8")
HOST_H = (ROOT / "src/mcu_stm32h757/CM7/DsiHost.h").read_text(encoding="utf-8")


class Mapping(unittest.TestCase):
    def test_every_divisor_and_coding_the_plan_can_give_has_a_hal_value(self):
        for idf in range(1, dp.IDF_MAX + 1):
            self.assertIn("DSI_PLL_IN_DIV%d" % idf, HOST_C)
        for odf in dp.ODFS:
            self.assertIn("DSI_PLL_OUT_DIV%d" % odf, HOST_C)
        for coding in ("DSI_RGB565", "DSI_RGB666", "DSI_RGB888"):
            self.assertIn(coding, HOST_C)
        self.assertEqual(dp.BPPS, (16, 18, 24))
        for lanes in ("DSI_ONE_DATA_LANE", "DSI_TWO_DATA_LANES"):
            self.assertIn(lanes, HOST_C)
        self.assertEqual(dp.LANES_MAX, 2)

    def test_the_idf_table_is_in_order(self):
        table = re.search(r"g_idf\[8\] = \{0u, (.*?)\};", HOST_C).group(1)
        self.assertEqual(re.findall(r"DSI_PLL_IN_DIV(\d)", table), [str(i) for i in range(1, 8)])

    def test_the_escape_clock_stays_under_20_mhz(self):
        link = dp.load_config()["link"]
        made = 0
        for rate in range(62_500, 1_000_001, 12_500):
            try:
                plan = dp.make_plan({**link, "lane_kbps": rate, "tolerance_permille": 50, "width": 320, "height": 240, "fps": 30})
            except ValueError:
                continue
            made += 1
            divider = -(-plan["byte_clock_khz"] // 20_000)
            self.assertLessEqual(plan["byte_clock_khz"] / divider, 20_000)
            self.assertGreaterEqual(divider, 1)
        self.assertGreater(made, 40)
        self.assertIn("#define DSIH_ESCAPE_MAX_KHZ 20000u", HOST_C)

    def test_the_defaults_give_an_escape_divider_of_four(self):
        plan = dp.make_plan(dp.load_config()["link"])
        self.assertEqual(-(-plan["byte_clock_khz"] // 20_000), 4)


class Sources(unittest.TestCase):
    def test_the_switch_ships_off_and_the_whole_unit_is_inside_it(self):
        self.assertIn("#define HYDRA_DSI_HOST 0", HOST_H)
        self.assertTrue(HOST_C.rstrip().endswith("#endif /* HYDRA_DSI_HOST */"))
        self.assertEqual(HOST_C.count("#if HYDRA_DSI_HOST"), 1)

    def test_the_panel_reset_pin_is_not_invented(self):
        self.assertNotIn("HAL_GPIO", HOST_C)
        self.assertIn("reset line of the panel is not driven here", HOST_H)

    def test_the_new_source_is_in_both_runners_and_has_a_floor(self):
        host = (ROOT / "tools/host_tests.py").read_text(encoding="utf-8")
        build = (ROOT / "tools/build_firmware.py").read_text(encoding="utf-8")
        self.assertIn("tests/test_dsi_host.c", host)
        for token in ("test_dsi_host", "-DHYDRA_DSI_HOST=1", "DsiHost.c"):
            self.assertIn(token, build)
        floors = json.loads((ROOT / "tools/host_coverage_floor.json").read_text(encoding="utf-8"))["modules"]
        self.assertEqual(floors["src/mcu_stm32h757/CM7/DsiHost.c"], 100)


if __name__ == "__main__":
    unittest.main()
