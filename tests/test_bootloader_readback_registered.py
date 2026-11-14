# =============================================================================
# HYDRA-UMC - tests/test_bootloader_readback_registered.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The host tests of the G474 read-back, the validity check and the queries (both flavours of the read-out gate) and of the H745 flash write, copy and
metadata log (both cores, with and without the flash layer) are in both lists that run the C tests.

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
FLASH_SOURCE = "tests/test_bootloader_h745_flash_host.c"


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

    def test_the_h745_flash_test_runs_for_both_cores_with_and_without_the_flash_layer(self):
        found = {test.name: test for test in host_tests.table() if test.source == FLASH_SOURCE}
        self.assertEqual(
            sorted(found),
            [f"bootloader_h745_{core}_flash_ops{layer}" for core in ("CM4", "CM7") for layer in ("", "_flash_layer")],
        )
        for name, test in found.items():
            self.assertIn("tests/stubs", test.includes)
            self.assertEqual("HYDRA_FLASH_LAYER=1" in test.defines, name.endswith("_flash_layer"), name)
            self.assertEqual(sum(d.startswith("HYDRA_TEST_CM") for d in test.defines), 1, name)
        compiles = [c for case in build_firmware.bootloader_cases("root") for c in case.compiles if c.test == "test_bootloader_h745_flash_host"]
        self.assertEqual(len(compiles), 4)
        self.assertEqual(sum("-DHYDRA_FLASH_LAYER=1" in c.defines for c in compiles), 2)

    def test_the_h745_flash_test_names_what_it_covers(self):
        text = (ROOT / FLASH_SOURCE).read_text(encoding="utf-8")
        for name in ("Flash_WriteVerified", "Flash_CopyRegion", "Metadata_EraseAndWrite", "Metadata_Read", "MAP_32BIT"):
            self.assertIn(name, text)
        self.assertEqual(text.count('#include "bootloader_flash.c"'), 1)

    def test_the_gate_is_a_switch_the_firmware_ships_off(self):
        guard = (ROOT / "src" / "mcu_stm32g474" / "boot" / "adoption_guard.h").read_text(encoding="utf-8")
        self.assertIn("#ifndef HYDRA_READBACK_GATE", guard)
        self.assertIn("#define HYDRA_READBACK_GATE 0", guard)


if __name__ == "__main__":
    unittest.main()
