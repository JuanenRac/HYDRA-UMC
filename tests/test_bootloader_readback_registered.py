# =============================================================================
# HYDRA-UMC - tests/test_bootloader_readback_registered.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The host test of the G474 read-back, the validity check and the queries is in both lists that run the C tests, in both flavours of the read-out gate.

Nothing here compiles: it reads the two tables and the C file, so a test that was written but never wired in (or wired into only one list) fails here
on any machine, not only where a C compiler runs it.
"""
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import build_firmware  # noqa: E402
import host_tests  # noqa: E402

SOURCE = "tests/test_bootloader_g474_readback_host.c"


class Registered(unittest.TestCase):
    def test_the_host_tests_table_has_both_flavours_of_the_gate(self):
        found = {test.name: test for test in host_tests.table() if test.source == SOURCE}
        self.assertEqual(sorted(found), ["bootloader_g474_readback@gate0", "bootloader_g474_readback@gate1"])
        for gate in (0, 1):
            test = found[f"bootloader_g474_readback@gate{gate}"]
            self.assertIn(f"HYDRA_READBACK_GATE={gate}", test.defines)
            self.assertTrue(test.needs_tweetnacl)
            self.assertIn("src/mcu_stm32g474/boot/update_signature.c", test.extra_sources)
            self.assertIn("tests/stubs/protocol", test.includes)

    def test_the_build_script_runs_the_same_two_flavours(self):
        compiles = [c for case in build_firmware.bootloader_cases("root") for c in case.compiles if c.test == "test_bootloader_g474_readback_host"]
        self.assertEqual(len(compiles), 2)
        self.assertEqual(sorted(c.defines for c in compiles), [("-DHYDRA_READBACK_GATE=0",), ("-DHYDRA_READBACK_GATE=1",)])

    def test_the_c_file_exists_and_names_what_it_covers(self):
        text = (ROOT / SOURCE).read_text(encoding="utf-8")
        for name in ("HandleReadbackStart", "ApplicationIsValid", "HandleVersionQuery", "HandleErrorCounterQuery", "JumpToApplication"):
            self.assertIn(name, text)
        self.assertEqual(text.count("#include \"bootloader_protocol.c\""), 1)

    def test_the_gate_is_a_switch_the_firmware_ships_off(self):
        guard = (ROOT / "src" / "mcu_stm32g474" / "boot" / "adoption_guard.h").read_text(encoding="utf-8")
        self.assertIn("#ifndef HYDRA_READBACK_GATE", guard)
        self.assertIn("#define HYDRA_READBACK_GATE 0", guard)


if __name__ == "__main__":
    unittest.main()
