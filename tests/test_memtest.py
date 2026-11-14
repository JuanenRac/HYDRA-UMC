# =============================================================================
# HYDRA-UMC - tests/test_memtest.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The memory tests for the external SDRAM (memtest.h) are free of the HAL, reach the memory only through two callbacks, run the data bus, the address bus and the march in that
order, and their host test is registered in both runners. The tests are run by tests/test_memtest.c against a RAM double with faults. Evidence: simulator (text checks)."""
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "src/mcu_stm32h757/Common/memtest.h").read_text(encoding="utf-8")


class MemtestTests(unittest.TestCase):
    def test_the_header_is_free_of_the_hal_and_uses_only_the_callbacks(self):
        self.assertNotIn("HAL_", HEADER)
        self.assertNotIn("0xC0000000", HEADER)   # no address: the caller gives the accessors
        self.assertIn("uint32_t (*read)(void *ctx, uint32_t word_index);", HEADER)
        self.assertIn("void (*write)(void *ctx, uint32_t word_index, uint32_t value);", HEADER)

    def test_the_run_goes_data_bus_then_address_bus_then_march_and_refuses_a_bad_window_first(self):
        run = HEADER.split("static inline MemtestStatus Memtest_Run(", 1)[1]
        self.assertLess(run.index("MEMTEST_BAD_WINDOW"), run.index("Memtest_DataBus"))
        self.assertLess(run.index("Memtest_DataBus"), run.index("Memtest_AddressBus"))
        self.assertLess(run.index("Memtest_AddressBus"), run.index("Memtest_March"))

    def test_the_march_has_the_six_elements_of_march_c_minus(self):
        march = HEADER.split("static inline MemtestStatus Memtest_March(", 1)[1].split("\nstatic inline", 1)[0]
        for element in ("up(w0)", "up(r0, w1)", "up(r1, w0)", "down(r0, w1)", "down(r1, w0)", "down(r0)"):
            self.assertIn(element, march)

    def test_the_host_test_is_registered_in_both_runners(self):
        self.assertIn("tests/test_memtest.c", (ROOT / "tools/host_tests.py").read_text(encoding="utf-8"))
        self.assertIn("test_memtest", (ROOT / "tools" / "build_firmware.py").read_text(encoding="utf-8"))

    def test_the_temperature_test_is_not_claimed(self):
        self.assertIn("temperature test of the guide is not here", HEADER)


if __name__ == "__main__":
    unittest.main()
