# =============================================================================
# HYDRA-UMC - tests/test_expansion_slots.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The power sequence and fault handling of the expansion slots: the Python reference, its configuration, the lines it drives (taken from the platform profile) and the vectors the C twin is
held to. Evidence: simulator (nothing has run on a board)."""
import random
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import expansion_slots as ex  # noqa: E402
import gen_expansion_slots_vectors as gen  # noqa: E402
import platform_profile as pp  # noqa: E402

HEADER = (ROOT / "src/mcu_stm32h757/Common/expansion_slots.h").read_text(encoding="utf-8")


def model(**changes):
    cfg = dict(ex.load_config(), allowed_mask=7)
    cfg.update(changes)
    m = ex.ExpansionSlots(cfg)
    m.start(0)
    return m


def run_until(m, start, end, present=0, fault=0, step=5):
    for t in range(start, end + 1, step):
        m.update(t, present, fault)


class ConfigTests(unittest.TestCase):
    def test_the_file_powers_nothing_by_default(self):
        cfg = ex.load_config()
        self.assertEqual(cfg["allowed_mask"], 0)
        m = ex.ExpansionSlots(cfg)
        m.start(0)
        run_until(m, 0, 500, present=0b111)
        self.assertEqual([s.enable for s in m.slots], [False] * 3)
        self.assertEqual([s.state for s in m.slots], [ex.ST_OFF] * 3)

    def test_values_out_of_range_are_refused(self):
        base = (ROOT / "config/expansion_slots.cfg").read_text(encoding="utf-8")
        changes = (
            ("allowed_mask = 0", "allowed_mask = 8"),
            ("fault_blank_ms = 20", "fault_blank_ms = 0"),
            ("power_good_ms = 50", "power_good_ms = 20"),
            ("reset_release_ms = 20", "reset_release_ms = 0"),
            ("fault_debounce_ms = 5", "fault_debounce_ms = 2000"),
            ("max_faults = 3", "max_faults = 0"),
        )
        for field, bad in changes:
            self.assertIn(field, base)
            with tempfile.TemporaryDirectory() as folder:
                path = Path(folder) / "expansion_slots.cfg"
                path.write_text(base.replace(field, bad), encoding="utf-8")
                with self.assertRaises(ValueError, msg=bad):
                    ex.load_config(path)

    def test_the_c_twin_refuses_the_same_limits(self):
        for fragment in ("allowed_mask >= (1u << EXP_SLOTS)", "fault_blank_ms > 1000u", "power_good_ms <= cfg->fault_blank_ms", "power_good_ms > 5000u", "max_faults > 20u"):
            self.assertIn(fragment, HEADER)


class SequenceTests(unittest.TestCase):
    def test_power_comes_before_the_release_of_reset_and_ready_comes_last(self):
        m = model()
        m.update(0, 0, 0)
        m.update(5, 0b001, 0)
        self.assertTrue(m.slots[0].enable)
        self.assertFalse(m.slots[0].reset_released)
        self.assertEqual(m.slots[0].state, ex.ST_POWERING)
        m.update(54, 0b001, 0)
        self.assertFalse(m.slots[0].reset_released)
        m.update(55, 0b001, 0)
        self.assertTrue(m.slots[0].reset_released)
        self.assertEqual(m.slots[0].state, ex.ST_RESETTING)
        m.update(75, 0b001, 0)
        self.assertEqual(m.slots[0].state, ex.ST_READY)

    def test_the_inrush_is_ignored_but_a_fault_after_it_cuts_the_slot(self):
        m = model()
        m.update(0, 0b001, 0)
        m.update(10, 0b001, 0b001)
        self.assertEqual(m.slots[0].state, ex.ST_POWERING)
        m.update(21, 0b001, 0b001)
        self.assertEqual(m.slots[0].state, ex.ST_FAULT)
        self.assertFalse(m.slots[0].enable)
        self.assertFalse(m.slots[0].reset_released)

    def test_a_fault_stays_until_a_person_clears_it_and_the_fuse_is_quiet(self):
        m = model()
        run_until(m, 0, 100, present=1)
        run_until(m, 105, 125, present=1, fault=1)
        self.assertEqual(m.slots[0].state, ex.ST_FAULT)
        run_until(m, 130, 200, present=1)
        self.assertEqual(m.slots[0].state, ex.ST_FAULT)  # no one cleared it
        m.clear_fault(0)
        m.update(205, 1, 1)
        self.assertEqual(m.slots[0].state, ex.ST_FAULT)  # cleared but the fuse still reports a fault
        m.update(210, 1, 0)
        self.assertNotEqual(m.slots[0].state, ex.ST_FAULT)

    def test_a_slot_that_faults_too_often_is_locked_until_a_person_unlocks_it(self):
        m = model(max_faults=2, fault_debounce_ms=0)
        for _ in range(2):
            run_until(m, m.slots[0].since, m.slots[0].since + 100, present=1)
            t = m.slots[0].since + 100
            m.update(t, 1, 1)
            m.clear_fault(0)
            m.update(t + 5, 1, 0)
        self.assertEqual(m.slots[0].state, ex.ST_LOCKED)
        m.clear_fault(0)
        m.update(1000, 1, 0)
        self.assertEqual(m.slots[0].state, ex.ST_LOCKED)
        m.unlock(0, 1005)
        m.update(1010, 1, 0)
        self.assertEqual(m.slots[0].faults, 0)
        self.assertTrue(m.slots[0].enable)

    def test_pulling_the_card_cuts_the_slot_at_once(self):
        m = model()
        run_until(m, 0, 150, present=0b100)
        self.assertEqual(m.slots[2].state, ex.ST_READY)
        m.update(155, 0, 0)
        self.assertEqual(m.slots[2].state, ex.ST_ABSENT)
        self.assertFalse(m.slots[2].enable)

    def test_slots_are_independent(self):
        m = model()
        run_until(m, 0, 40, present=0b001)
        run_until(m, 45, 200, present=0b011, fault=0b010)
        self.assertEqual(m.slots[0].state, ex.ST_READY)
        self.assertIn(m.slots[1].state, (ex.ST_FAULT, ex.ST_ABSENT, ex.ST_POWERING))
        self.assertEqual(m.slots[2].state, ex.ST_ABSENT)

    def test_two_safety_rules_hold_over_a_long_random_run(self):
        rng = random.Random(5)
        for allowed in (0b000, 0b101, 0b111):
            m = model(allowed_mask=allowed)
            t = 0
            present = fault = 0
            for _ in range(3000):
                t += rng.choice([1, 2, 5, 20])
                if rng.random() < 0.1:
                    present ^= 1 << rng.randrange(3)
                if rng.random() < 0.15:
                    fault ^= 1 << rng.randrange(3)
                if rng.random() < 0.05:
                    m.clear_fault(rng.randrange(3))
                if rng.random() < 0.02:
                    m.unlock(rng.randrange(3), t)
                m.update(t, present, fault)
                for index, slot in enumerate(m.slots):
                    self.assertFalse(slot.reset_released and not slot.enable, "a card out of reset without power")
                    if not allowed >> index & 1:
                        self.assertFalse(slot.enable, "a slot that is not allowed is powered")
                    if slot.enable:
                        self.assertTrue(present >> index & 1, "a slot is powered without a card")


class ProfileTests(unittest.TestCase):
    def test_the_slots_of_the_model_are_the_slots_of_the_platform_profile(self):
        profile = pp.load()["profiles"]["h757_target"]
        if "expansion" not in profile:
            self.skipTest("the platform profile does not list the expansion slots yet")
        self.assertEqual(sorted(profile["expansion"]["slots"]), [str(n) for n in range(1, ex.SLOTS + 1)])
        for lines in profile["expansion"]["slots"].values():
            self.assertEqual(sorted(lines), ["enable", "fault_n", "irq", "present_n", "reset_n"])


class TwinTests(unittest.TestCase):
    def test_the_vectors_are_what_the_reference_says(self):
        self.assertEqual(gen.main(["--check"]), 0)

    def test_every_state_appears_in_the_scenarios(self):
        seen = set()
        for cfg_name, steps in gen.SCENARIOS.values():
            for row in gen.run(cfg_name, steps):
                seen.update(row[0:3])
        self.assertEqual(seen, {ex.ST_OFF, ex.ST_ABSENT, ex.ST_POWERING, ex.ST_RESETTING, ex.ST_READY, ex.ST_FAULT, ex.ST_LOCKED})

    def test_the_c_test_is_in_both_lists_and_the_header_has_no_hal(self):
        self.assertIn("tests/test_expansion_slots.c", (ROOT / "tools/host_tests.py").read_text(encoding="utf-8"))
        self.assertIn("test_expansion_slots.c", (ROOT / "build_firmware.sh").read_text(encoding="utf-8"))
        self.assertNotIn("HAL_", HEADER)


if __name__ == "__main__":
    unittest.main()
