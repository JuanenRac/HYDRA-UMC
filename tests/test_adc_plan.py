# =============================================================================
# HYDRA-UMC - tests/test_adc_plan.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The ADC of the CM7: the Python twin (tools/adc_plan.py) gives the same conversions as tests/test_adc_plan.c, config/adc.cfg covers the analog inputs of the platform profile and
has no pin twice, the limits are the same in C and Python, the switch ships off, and the new sources are in both runners with floors. Evidence: simulator (the ADC double runs in
tests/test_adc_reader.c)."""
import json
import re
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import adc_plan as ap  # noqa: E402

PLAN_H = (ROOT / "src/mcu_stm32h757/Common/adc_plan.h").read_text(encoding="utf-8")
READER_C = (ROOT / "src/mcu_stm32h757/CM7/AdcReader.c").read_text(encoding="utf-8")
READER_H = (ROOT / "src/mcu_stm32h757/CM7/AdcReader.h").read_text(encoding="utf-8")


def macro(name):
    return int(re.search(rf"#define {name} (\d+)u?L?", PLAN_H).group(1))


CFG = {"vref_mv": 3300, "bits": 16, "samples": 8, "timeout_ms": 10, "sample_code": 1}


def channel(**changes):
    base = {"adc": 1, "channel": 16, "num": 1, "den": 1, "offset_mv": 0}
    base.update(changes)
    return base


class Conversions(unittest.TestCase):
    """The same numbers as tests/test_adc_plan.c."""

    def test_the_conversions(self):
        cases = [
            (CFG, channel(), 0, 0),
            (CFG, channel(), 65535, 3300),
            (CFG, channel(), 32768, 1650),
            ({**CFG, "bits": 12}, channel(), 4095, 3300),
            ({**CFG, "bits": 12}, channel(), 1024, 825),
            (CFG, channel(num=11), 32768, 18150),
            (CFG, channel(num=1, den=2, offset_mv=-250), 32768, 575),
            (CFG, channel(num=1, den=3), 65535, 1100),
            (CFG, channel(), 1, 0),
            (CFG, channel(), 20, 1),
            (CFG, channel(offset_mv=-500), 0, -500),
        ]
        for cfg, ch, raw, expected in cases:
            self.assertEqual(ap.to_millivolts(cfg, ch, raw), expected, (cfg, ch, raw))
        source = (ROOT / "tests/test_adc_plan.c").read_text(encoding="utf-8")
        for needle in ("18150", "575", "1100", "825", "-500"):
            self.assertIn(needle, source)

    def test_the_average(self):
        self.assertEqual([ap.average(80, 8), ap.average(83, 8), ap.average(84, 8), ap.average(7, 1)], [10, 10, 11, 7])

    def test_refusals(self):
        for cfg, ch, raw in (
            ({**CFG, "bits": 12}, channel(), 4096),
            (CFG, channel(num=1_000_000), 65535),
            ({**CFG, "vref_mv": 1799}, channel(), 1),
            ({**CFG, "vref_mv": 3601}, channel(), 1),
            ({**CFG, "bits": 14}, channel(), 1),
            ({**CFG, "samples": 0}, channel(), 1),
            ({**CFG, "samples": 65}, channel(), 1),
            ({**CFG, "timeout_ms": 0}, channel(), 1),
            ({**CFG, "timeout_ms": 1001}, channel(), 1),
            ({**CFG, "sample_code": 3}, channel(), 1),
            (CFG, channel(adc=0), 1),
            (CFG, channel(adc=4), 1),
            (CFG, channel(channel=20), 1),
            (CFG, channel(num=0), 1),
            (CFG, channel(den=0), 1),
            (CFG, channel(num=1_000_001), 1),
            (CFG, channel(den=1_000_001), 1),
            (CFG, channel(offset_mv=100_001), 1),
            (CFG, channel(offset_mv=-100_001), 1),
        ):
            with self.assertRaises(ValueError, msg=str((cfg, ch, raw))):
                ap.to_millivolts(cfg, ch, raw)


class ConfigAndSources(unittest.TestCase):
    def test_the_file_is_valid(self):
        data = ap.load_config()
        self.assertEqual(data["adc"], {"vref_mv": 3300, "bits": 16, "samples": 8, "timeout_ms": 10, "sample_code": 1})
        self.assertEqual(len(data["channels"]), 18)

    def test_every_analog_input_of_the_profile_is_in_the_file(self):
        board_io = json.loads((ROOT / "docs/platform_profiles.json").read_text(encoding="utf-8"))
        text = json.dumps(board_io)
        names = ["v24", "jetson", "usb5v", "m2_5v", "hailo", "nvme", "wifi", "fan5v", "ctrl3v3", "ctrl5v", "i_v24_total", "i_jetson", "i_usb", "i_m2", "i_ctrl", "i_fan", "ai1", "ai2"]
        self.assertEqual(sorted(ap.load_config()["channels"]), sorted(names))
        for pin in ("PA0_C", "PA1_C", "PA0", "PA1", "PA2", "PA3", "PA6", "PA7", "PC3_C", "PC2_C", "PB0", "PB1", "PC1", "PC4", "PC5", "PH3", "PH2", "PF10"):
            self.assertIn('"%s"' % pin, text)

    def test_no_input_is_on_the_same_adc_channel_twice_and_the_unassigned_ones_are_listed(self):
        channels = ap.load_config()["channels"]
        used = [(c["adc"], c["channel"]) for c in channels.values() if c["adc"] != 0]
        self.assertEqual(len(used), len(set(used)))
        self.assertEqual(sorted(name for name, c in channels.items() if c["adc"] == 0), ["ai1", "i_fan"])

    def test_the_limits_are_the_same_in_c(self):
        self.assertEqual((macro("ADCP_VREF_MIN_MV"), macro("ADCP_VREF_MAX_MV")), ap.VREF_MV)
        self.assertEqual((macro("ADCP_SAMPLES_MIN"), macro("ADCP_SAMPLES_MAX")), ap.SAMPLES)
        self.assertEqual((macro("ADCP_TIMEOUT_MIN_MS"), macro("ADCP_TIMEOUT_MAX_MS")), ap.TIMEOUT_MS)
        self.assertEqual((macro("ADCP_ADC_MAX"), macro("ADCP_CHANNEL_MAX")), (ap.ADC_MAX, ap.CHANNEL_MAX))
        self.assertEqual((macro("ADCP_RATIO_MAX"), macro("ADCP_OFFSET_MAX_MV")), (ap.RATIO_MAX, ap.OFFSET_MAX_MV))
        self.assertEqual(macro("ADCP_SAMPLE_CODE_MAX"), max(ap.SAMPLE_CODES.values()))
        self.assertIn("c->bits != 12u && c->bits != 16u", PLAN_H)

    def test_the_sampling_times_are_the_ones_of_the_codes(self):
        self.assertIn("{ADC_SAMPLETIME_16CYCLES_5, ADC_SAMPLETIME_64CYCLES_5, ADC_SAMPLETIME_387CYCLES_5}", READER_C)
        self.assertEqual(ap.SAMPLE_CODES, {"16.5": 0, "64.5": 1, "387.5": 2})

    def test_a_bad_file_is_refused(self):
        base = (ROOT / "config/adc.cfg").read_text(encoding="utf-8").replace("\r\n", "\n")
        for good, bad in (("bits = 16", "bits = 14"), ("sample_time = 64.5", "sample_time = 10"), ("ctrl5v = 3, 0, 1, 1, 0", "ctrl5v = 3, 20, 1, 1, 0"), ("ai2 = 3, 6, 1, 1, 0", "ai2 = 3, 6, 1, 1"), ("jetson = 1, 1, 1, 1, 0", "jetson = 1, 1, 1, 0, 0")):
            text = base.replace(good, bad, 1)
            self.assertNotEqual(text, base, bad)
            with tempfile.TemporaryDirectory() as folder:
                path = Path(folder) / "adc.cfg"
                path.write_text(text, encoding="utf-8")
                with self.assertRaises(ValueError, msg=bad):
                    ap.load_config(path)

    def test_the_switch_ships_off_and_the_whole_unit_is_inside_it(self):
        self.assertIn("#define HYDRA_ADC_CM7 0", READER_H)
        self.assertTrue(READER_C.rstrip().endswith("#endif /* HYDRA_ADC_CM7 */"))
        self.assertEqual(READER_C.count("#if HYDRA_ADC_CM7"), 1)

    def test_the_new_sources_are_in_both_runners_and_have_floors(self):
        host = (ROOT / "tools/host_tests.py").read_text(encoding="utf-8")
        build = (ROOT / "tools/build_firmware.py").read_text(encoding="utf-8")
        for test in ("tests/test_adc_plan.c", "tests/test_adc_reader.c"):
            self.assertIn(test, host)
        for token in ("test_adc_plan", "test_adc_reader", "-DHYDRA_ADC_CM7=1", "AdcReader.c"):
            self.assertIn(token, build)
        floors = json.loads((ROOT / "tools/host_coverage_floor.json").read_text(encoding="utf-8"))["modules"]
        for module in ("Common/adc_plan.h", "CM7/AdcReader.c"):
            self.assertEqual(floors["src/mcu_stm32h757/" + module], 100)


if __name__ == "__main__":
    unittest.main()
