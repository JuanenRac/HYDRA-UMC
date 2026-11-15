# =============================================================================
# HYDRA-UMC - tests/test_dac_plan.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The analog outputs of the CM7: the Python twin (tools/dac_plan.py) gives the same codes as tests/test_dac_plan.c, config/dac.cfg is valid and its pins are the ones of the platform
profile, the limits are the same in C and Python, the outputs start at 0 V, the switch ships off, and the new sources are in both runners with floors. Evidence: simulator (the DAC
double runs in tests/test_dac_out.c)."""
import json
import re
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import dac_plan as dp  # noqa: E402

PLAN_H = (ROOT / "src/mcu_stm32h757/Common/dac_plan.h").read_text(encoding="utf-8")
OUT_C = (ROOT / "src/mcu_stm32h757/CM7/DacOut.c").read_text(encoding="utf-8")
OUT_H = (ROOT / "src/mcu_stm32h757/CM7/DacOut.h").read_text(encoding="utf-8")


def macro(name):
    return int(re.search(rf"#define {name} (\d+)", PLAN_H).group(1))


def cfg(**changes):
    base = {"vref_mv": 3300, "gain_num": 3, "gain_den": 1, "ceiling_mv": [9900, 9900]}
    base.update(changes)
    return base


class Codes(unittest.TestCase):
    """The same numbers as tests/test_dac_plan.c."""

    def test_the_codes(self):
        cases = [
            (cfg(), 1, 0, 0),
            (cfg(), 1, 9900, 4095),
            (cfg(), 2, 4950, 2048),
            (cfg(), 1, 1000, 414),
            (cfg(), 1, 1, 0),
            (cfg(), 1, 3, 1),
            (cfg(vref_mv=2500, gain_num=4, ceiling_mv=[10000, 9900]), 1, 10000, 4095),
            (cfg(vref_mv=2500, gain_num=10, gain_den=3, ceiling_mv=[10000, 9900]), 1, 5000, 2457),
        ]
        for config, channel, mv, expected in cases:
            self.assertEqual(dp.code_for(config, channel, mv), expected, (config, channel, mv))
        source = (ROOT / "tests/test_dac_plan.c").read_text(encoding="utf-8")
        for needle in ("414u", "2457u", "2048u", "4095u"):
            self.assertIn(needle, source)

    def test_refusals(self):
        for config, channel, mv in (
            (cfg(), 1, 9901),
            (cfg(ceiling_mv=[9900, 5000]), 2, 5001),
            (cfg(ceiling_mv=[10000, 9900]), 1, 10000),
            (cfg(), 0, 1),
            (cfg(), 3, 1),
            (cfg(vref_mv=1799), 1, 1),
            (cfg(vref_mv=3601), 1, 1),
            (cfg(gain_num=0), 1, 1),
            (cfg(gain_den=0), 1, 1),
            (cfg(gain_num=1001), 1, 1),
            (cfg(gain_den=1001), 1, 1),
            (cfg(ceiling_mv=[10001, 9900]), 1, 1),
            (cfg(ceiling_mv=[9900, 10001]), 1, 1),
        ):
            with self.assertRaises(ValueError, msg=str((config, channel, mv))):
                dp.code_for(config, channel, mv)

    def test_a_code_never_differs_from_the_request_by_more_than_half_a_step(self):
        config = cfg()
        step = 3300 / 4095 * 3
        for mv in range(0, 9901, 7):
            back = dp.code_for(config, 1, mv) * step
            self.assertLessEqual(abs(back - mv), step / 2 + 1e-6, mv)


class ConfigAndSources(unittest.TestCase):
    def test_the_file_is_valid(self):
        self.assertEqual(dp.load_config(), cfg())

    def test_the_pins_are_those_of_the_profile(self):
        text = json.dumps(json.loads((ROOT / "docs/platform_profiles.json").read_text(encoding="utf-8")))
        self.assertIn('"ao1_0_10v": "PA4"', text)
        self.assertIn('"ao2_0_10v": "PA5"', text)

    def test_the_limits_are_the_same_in_c(self):
        self.assertEqual((macro("DACP_VREF_MIN_MV"), macro("DACP_VREF_MAX_MV")), dp.VREF_MV)
        self.assertEqual((macro("DACP_RATIO_MAX"), macro("DACP_OUT_MAX_MV")), (dp.RATIO_MAX, dp.OUT_MAX_MV))
        self.assertEqual((macro("DACP_CODE_MAX"), macro("DACP_CHANNELS")), (dp.CODE_MAX, dp.CHANNELS))

    def test_a_bad_file_is_refused(self):
        base = (ROOT / "config/dac.cfg").read_text(encoding="utf-8").replace("\r\n", "\n")
        for good, bad in (("vref_mv = 3300", "vref_mv = 1000"), ("gain_num = 3", "gain_num = 0"), ("ao1_mv = 9900", "ao1_mv = 10001"), ("ao2_mv = 9900", "ao2_mv = 12000")):
            text = base.replace(good, bad, 1)
            self.assertNotEqual(text, base, bad)
            with tempfile.TemporaryDirectory() as folder:
                path = Path(folder) / "dac.cfg"
                path.write_text(text, encoding="utf-8")
                with self.assertRaises(ValueError, msg=bad):
                    dp.load_config(path)

    def test_the_outputs_start_at_zero_and_the_value_is_written_before_the_start(self):
        loop = OUT_C[OUT_C.index("for (unsigned i = 0u; i < DACP_CHANNELS; i++)"):]
        self.assertLess(loop.index("HAL_DAC_SetValue(&g_dac, g_channel[i], DAC_ALIGN_12B_R, 0u)"), loop.index("HAL_DAC_Start"))

    def test_the_switch_ships_off_and_the_whole_unit_is_inside_it(self):
        self.assertIn("#define HYDRA_DAC_CM7 0", OUT_H)
        self.assertTrue(OUT_C.rstrip().endswith("#endif /* HYDRA_DAC_CM7 */"))
        self.assertEqual(OUT_C.count("#if HYDRA_DAC_CM7"), 1)

    def test_the_new_sources_are_in_both_runners_and_have_floors(self):
        host = (ROOT / "tools/host_tests.py").read_text(encoding="utf-8")
        build = (ROOT / "tools/build_firmware.py").read_text(encoding="utf-8")
        for test in ("tests/test_dac_plan.c", "tests/test_dac_out.c"):
            self.assertIn(test, host)
        for token in ("test_dac_plan", "test_dac_out", "-DHYDRA_DAC_CM7=1", "DacOut.c"):
            self.assertIn(token, build)
        floors = json.loads((ROOT / "tools/host_coverage_floor.json").read_text(encoding="utf-8"))["modules"]
        for module in ("Common/dac_plan.h", "CM7/DacOut.c"):
            self.assertEqual(floors["src/mcu_stm32h757/" + module], 100)


if __name__ == "__main__":
    unittest.main()
