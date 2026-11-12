# =============================================================================
# HYDRA-UMC - tests/test_ui_pin.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The store of the operator PIN and the pad that types it (tools/ui_pin.py): the digits are never kept, a record that cannot be read never lets a PIN in, a power cut in the
middle of a write leaves the old record, the failure counter survives a restart, and the pad with the view-model behaves as the design says. Evidence: simulator; the cost of the
hash on the Cortex-M7 is not measured."""
import inspect
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import ui_model as model  # noqa: E402
import ui_pin as pin  # noqa: E402

FAST = {"pin_hash_iterations": 1000, "pin_min_digits": 4, "pin_max_digits": 8, "pin_cycles_per_block": 2600, "pin_hw_cycles_per_block": 200, "pin_check_max_ms": 300}
SALT = bytes(range(16))


def store(settings=None):
    fram = pin.Fram()
    return pin.PinStore(fram, settings or FAST), fram


class StoreTests(unittest.TestCase):
    def test_the_settings_come_from_the_configuration_file(self):
        values = pin.load_settings()
        self.assertEqual(set(values), {"pin_hash_iterations", "pin_min_digits", "pin_max_digits", "pin_cycles_per_block", "pin_hw_cycles_per_block", "pin_check_max_ms"})
        self.assertGreaterEqual(values["pin_hash_iterations"], 1000)

    def test_a_new_store_has_no_pin_and_accepts_none(self):
        s, _ = store()
        self.assertEqual(s.state(), "unset")
        self.assertEqual(s.verify("1234"), pin.UNAVAILABLE)

    def test_a_pin_set_is_accepted_and_a_wrong_one_is_not(self):
        s, _ = store()
        s.set_pin("4821", SALT)
        self.assertEqual(s.state(), "set")
        self.assertEqual(s.verify("4821"), pin.OK)
        self.assertEqual(s.verify("4822"), pin.BAD)
        self.assertEqual(s.verify("48210"), pin.BAD)
        self.assertEqual(s.verify("12ab"), pin.BAD)
        self.assertEqual(s.verify(""), pin.BAD)

    def test_the_digits_are_not_in_the_record_and_the_salt_changes_the_hash(self):
        s1, f1 = store()
        s2, f2 = store()
        s1.set_pin("4821", SALT)
        s2.set_pin("4821", bytes(reversed(range(16))))
        self.assertNotIn(b"4821", bytes(f1.data))
        self.assertNotIn(bytes([4, 8, 2, 1]), bytes(f1.data))
        self.assertNotEqual(bytes(f1.data), bytes(f2.data))

    def test_a_pin_of_the_wrong_kind_is_refused_when_it_is_set(self):
        s, _ = store()
        for bad in ("123", "123456789", "12a4", "1111", "", "١٢٣٤"):
            with self.assertRaises(ValueError, msg=repr(bad)):
                s.set_pin(bad, SALT)
        with self.assertRaises(ValueError):
            s.set_pin("4821", b"short")

    def test_the_comparison_is_constant_time(self):
        self.assertIn("compare_digest", inspect.getsource(pin.PinStore.verify))

    def test_the_failure_counter_is_written_before_the_comparison_and_survives_a_restart(self):
        s, fram = store()
        s.set_pin("4821", SALT)
        for expected in (1, 2, 3):
            self.assertEqual(s.verify("0000"), pin.BAD)
            self.assertEqual(s.failures(), expected)
        again = pin.PinStore(fram, FAST)  # the power was cut and came back
        self.assertEqual(again.failures(), 3)
        self.assertEqual(again.verify("4821"), pin.OK)
        self.assertEqual(again.failures(), 0)  # a right PIN clears the count

    def test_a_record_that_cannot_be_read_never_lets_a_pin_in(self):
        s, fram = store()
        s.set_pin("4821", SALT)
        for index in range(0, 2 * pin.RECORD):
            damaged = pin.Fram()
            damaged.data = bytearray(fram.data)
            damaged.data[index] ^= 0x5A
            result = pin.PinStore(damaged, FAST).verify("0000")
            self.assertNotEqual(result, pin.OK, index)
        garbage = pin.Fram()
        garbage.data = bytearray(b"\x00" * (2 * pin.RECORD))
        broken = pin.PinStore(garbage, FAST)
        self.assertEqual((broken.state(), broken.verify("4821")), ("corrupt", pin.UNAVAILABLE))

    def test_a_power_cut_in_the_middle_of_a_write_leaves_a_record_that_works(self):
        for cut in range(0, pin.RECORD + 1, 3):
            s, fram = store()
            s.set_pin("4821", SALT)
            fram.cut_after = cut
            try:
                s.set_pin("9376", bytes([9] * 16))
                finished = True
            except pin.TornWrite:
                finished = False
            fresh = pin.PinStore(fram, FAST)
            old, new = fresh.verify("4821") == pin.OK, fresh.verify("9376") == pin.OK
            self.assertTrue(old or new, f"cut after {cut}: no PIN works")
            self.assertFalse(old and new, f"cut after {cut}: both work")
            if finished:
                self.assertTrue(new, cut)

    def test_a_power_cut_while_counting_a_failure_does_not_count_as_a_success(self):
        s, fram = store()
        s.set_pin("4821", SALT)
        fram.cut_after = 10
        self.assertEqual(s.verify("4821"), pin.UNAVAILABLE)
        self.assertEqual(pin.PinStore(fram, FAST).verify("4821"), pin.OK)  # the old record is intact

    def test_the_sequence_number_wraps_and_the_newer_record_still_wins(self):
        s, _ = store()
        s.set_pin("4821", SALT)
        base = s._current()[1]
        for sequence in (30000, 60000, 65535, 0, 1):
            s._write(pin.Record(base.digits, sequence, base.iterations, base.salt, base.hash, sequence % 7))
            self.assertEqual(s._current()[1].sequence, sequence)
        self.assertEqual(s.verify("4821"), pin.OK)

    def test_the_erase_leaves_the_store_as_new(self):
        s, _ = store()
        s.set_pin("4821", SALT)
        s.erase()
        self.assertEqual((s.state(), s.verify("4821")), ("unset", pin.UNAVAILABLE))


class PadTests(unittest.TestCase):
    def pad(self, set_pin="4821"):
        s, _ = store()
        if set_pin:
            s.set_pin(set_pin, SALT)
        return pin.PinPad(s), s

    def type(self, p, digits):
        event = None
        for d in digits:
            p.touch(f"pin_digit_{d}")
        event = p.touch("pin_enter")
        return event

    def test_the_pad_collects_digits_clears_and_reports_right_or_wrong(self):
        p, _ = self.pad()
        self.assertEqual(self.type(p, "4821"), ("PIN_OK", ()))
        self.assertEqual(self.type(p, "4820"), ("PIN_BAD", ()))
        p.touch("pin_digit_1")
        p.touch("pin_clear")
        self.assertEqual(p.digits, "")
        self.assertIsNone(self.type(p, "12"))  # too short to be a try: nothing is counted
        self.assertEqual(p.store.failures(), 1)

    def test_the_pad_keeps_at_most_the_longest_pin_and_forgets_after_enter(self):
        p, _ = self.pad()
        for _ in range(20):
            p.touch("pin_digit_7")
        self.assertEqual(len(p.digits), FAST["pin_max_digits"])
        p.touch("pin_enter")
        self.assertEqual(p.digits, "")
        self.assertIsNone(p.touch("pin_nothing"))

    def test_at_start_the_counted_failures_reach_the_view_model_and_a_power_cut_buys_no_tries(self):
        cfg = model.load_config()
        cfg["operator_pin_enabled"] = 1
        state = model.State(cfg)
        state._derive()
        p, s = self.pad()
        for _ in range(cfg["pin_max_failures"]):
            s.verify("0000")
        restarted = pin.PinPad(pin.PinStore(s.fram, FAST))  # the power was cut and came back
        name, args = restarted.boot_event()
        self.assertEqual((name, args), ("PIN_FAILURES", (cfg["pin_max_failures"],)))
        effects = state.event(0, name, *args)
        self.assertIn("pin_lockout", effects)
        state.event(100, "PIN_OK")
        self.assertTrue(state.locked())  # a right PIN is ignored during the lock-out
        fresh, _ = self.pad()
        self.assertEqual(fresh.boot_event(), ("PIN_FAILURES", (0,)))

    def test_a_store_that_cannot_be_read_reads_as_wrong_so_the_panel_stays_locked(self):
        p, _ = self.pad(set_pin=None)
        self.assertEqual(self.type(p, "4821"), ("PIN_BAD", ()))

    def test_with_the_view_model_the_panel_unlocks_with_the_right_pin_and_stop_never_needs_it(self):
        cfg = model.load_config()
        cfg["operator_pin_enabled"] = 1
        state = model.State(cfg)
        state._derive()
        p, _ = self.pad()
        now = 0

        def feed(event):
            if event is not None:
                state.event(now, event[0], *event[1])

        state.event(now, "HB", 0)
        now += 100
        self.assertIn("refused:jog_arm:locked", state.event(now, "DOWN", model.A_JOG_ARM))
        now += 100
        self.assertIn("stop", state.event(now, "DOWN", model.A_STOP))  # no PIN asked
        for _ in range(cfg["pin_max_failures"]):
            now += 100
            feed(self.type(p, "0000"))
        self.assertGreater(state.pin_lockout_until, now)
        now += 100
        feed(self.type(p, "4821"))  # right, but during the lock-out
        self.assertTrue(state.locked())
        now = state.pin_lockout_until + 100
        state.event(now, "HB", 0)
        feed(self.type(p, "4821"))
        self.assertFalse(state.locked())
        self.assertEqual(p.store.failures(), 0)


if __name__ == "__main__":
    unittest.main()
