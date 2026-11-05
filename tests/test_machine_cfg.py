"""config/machine.cfg and the header generated from it: defaults, validation and the checks that the stop is immediate unless a file says otherwise."""
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import machine_cfg  # noqa: E402

BASE = (ROOT / "config" / "machine.cfg").read_text(encoding="utf-8")


def load_text(text):
    with tempfile.TemporaryDirectory() as folder:
        path = Path(folder) / "machine.cfg"
        path.write_text(text, encoding="utf-8")
        return machine_cfg.load(path)


class MachineConfigTests(unittest.TestCase):
    def test_the_header_matches_the_file(self):
        self.assertEqual(machine_cfg.HEADER_PATH.read_text(encoding="utf-8"), machine_cfg.render(machine_cfg.load()))

    def test_every_axis_stops_at_once_by_default(self):
        cfg = machine_cfg.load()
        self.assertEqual(cfg["stop_categories"], [0] * 6)
        self.assertIn("#define MACHINE_STOP_CATEGORY1_MASK 0x00u", machine_cfg.render(cfg))

    def test_an_axis_can_be_set_apart(self):
        cfg = load_text(BASE.replace("axis3_category =", "axis3_category = 1"))
        self.assertEqual(cfg["stop_categories"], [0, 0, 1, 0, 0, 0])
        self.assertIn("0x04u", machine_cfg.render(cfg))

    def test_the_default_category_applies_to_axes_left_empty(self):
        cfg = load_text(BASE.replace("default_category = 0", "default_category = 1").replace("axis2_category =", "axis2_category = 0"))
        self.assertEqual(cfg["stop_categories"], [1, 0, 1, 1, 1, 1])

    def test_a_category_outside_zero_and_one_is_refused(self):
        with self.assertRaises(machine_cfg.ConfigError):
            load_text(BASE.replace("axis1_category =", "axis1_category = 2"))

    def test_the_input_pin_must_be_a_free_pin(self):
        with self.assertRaises(machine_cfg.ConfigError):
            load_text(BASE.replace("pin = 9", "pin = 4"))

    def test_the_input_is_on_and_normally_closed_by_default(self):
        cfg = machine_cfg.load()
        self.assertEqual(cfg["input_enabled"], 1)
        self.assertEqual(cfg["input_healthy_level"], 0)

    def test_polarity_and_pin_come_from_the_file(self):
        text = BASE.replace("port = C", "port = B").replace("pin = 9", "pin = 4").replace("healthy_level = 0", "healthy_level = 1")
        out = machine_cfg.render(load_text(text))
        self.assertIn("MACHINE_ESTOP_INPUT_PORT 'B'", out)
        self.assertIn("MACHINE_ESTOP_INPUT_PIN 4u", out)
        self.assertIn("MACHINE_ESTOP_INPUT_HEALTHY_LEVEL 1u", out)

    def test_read_out_protection_is_off_by_default(self):
        self.assertEqual(machine_cfg.load()["rdp_level"], 0)

    def test_read_out_protection_level_two_is_never_offered(self):
        with self.assertRaises(machine_cfg.ConfigError):
            load_text(BASE.replace("rdp_level = 0", "rdp_level = 2"))

    def test_the_source_has_no_literal_pin_for_the_input(self):
        source = (ROOT / "src" / "mcu_stm32g474" / "RobotControllerRelay.c").read_text(encoding="utf-8")
        self.assertIn("MACHINE_ESTOP_INPUT_PIN", source)
        self.assertIn("MACHINE_STOP_CATEGORY1_MASK", source)


if __name__ == "__main__":
    unittest.main()
