# =============================================================================
# HYDRA-UMC - tests/test_ui_io_c.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The C twins of the PIN store (src/ui_h757/core/ui_pin.c) and of the host side of the Jetson link (ui_link.c) are built for the PC and run against the vectors written from the
Python references, with the sanitizers; and built for the Cortex-M7, where their size is held to the limits of src/ui_h757/budget.json. With no host compiler, or where compiling on
this PC is locked (tools/local_build.py), the host tests skip and the CI runs them. Evidence: simulator."""
import json
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import host_tests  # noqa: E402
import ui_pin_cost  # noqa: E402

CORE = ROOT / "src" / "ui_h757" / "core"
TEST = ROOT / "src" / "ui_h757" / "test"
STRICT = ("-std=c11", "-Wall", "-Wextra", "-Werror")


class HostTests(unittest.TestCase):
    def setUp(self):
        self.compiler = host_tests.find_compiler()
        if self.compiler is None:
            self.skipTest("no host C compiler (or compiling on this PC is locked): the CI runs this test")
        self.work = Path(tempfile.mkdtemp(prefix="ui_io_c_"))
        self.addCleanup(shutil.rmtree, self.work, True)

    def build_and_run(self, name, sources, *extra):
        exe = self.work / (name + (".exe" if sys.platform == "win32" else ""))
        command = [self.compiler, *STRICT, "-O1", f"-I{CORE}", f"-I{TEST}", *extra, "-o", str(exe), *[str(s) for s in sources]]
        done = subprocess.run(command, capture_output=True, text=True, check=False)
        self.assertEqual(done.returncode, 0, done.stderr)
        run = subprocess.run([str(exe)], capture_output=True, text=True, check=False)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        return run.stdout

    def test_the_pin_store_matches_the_reference_and_survives_cuts_flips_and_the_wrap(self):
        out = self.build_and_run("ui_pin_test", [TEST / "ui_pin_test.c", CORE / "ui_pin.c"], "-DUI_PIN_COUNT_BLOCKS")
        self.assertIn("ui_pin: PASS", out)
        count = int(re.search(r"compressions for 1000 iterations: (\d+)", out).group(1))
        self.assertEqual(count, ui_pin_cost.compressions(1000))  # the formula of the estimate is the count of the C code

    def test_the_pin_store_is_clean_under_the_sanitizers(self):
        out = self.build_and_run("ui_pin_asan", [TEST / "ui_pin_test.c", CORE / "ui_pin.c"], "-DUI_PIN_COUNT_BLOCKS", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined")
        self.assertIn("ui_pin: PASS", out)

    def test_the_link_master_replays_every_transcript_of_the_reference(self):
        out = self.build_and_run("ui_link_test", [TEST / "ui_link_test.c", CORE / "ui_link.c", CORE / "ui_pin.c"])
        self.assertIn("ui_link: PASS", out)
        self.assertIn(f"{self.count_scenarios()} transcripts replayed in C", out)

    def test_the_link_master_is_clean_under_the_sanitizers(self):
        out = self.build_and_run("ui_link_asan", [TEST / "ui_link_test.c", CORE / "ui_link.c", CORE / "ui_pin.c"], "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined")
        self.assertIn("ui_link: PASS", out)

    @staticmethod
    def count_scenarios():
        import ui_gen_io

        return len(ui_gen_io.link_scenarios())


class ForTheBoardTests(unittest.TestCase):
    def test_the_twins_build_for_the_cortex_m7_and_fit_their_limits(self):
        arm, size = shutil.which("arm-none-eabi-gcc"), shutil.which("arm-none-eabi-size")
        if arm is None or size is None:
            self.skipTest("the ARM compiler is not on this PC: the CI job that has it measures the twins")
        limits = json.loads((ROOT / "src" / "ui_h757" / "budget.json").read_text(encoding="utf-8"))["measured_limits"]
        work = Path(tempfile.mkdtemp(prefix="ui_io_arm_"))
        self.addCleanup(shutil.rmtree, work, True)
        for name in ("ui_pin", "ui_link"):
            obj = work / f"{name}.o"
            command = [arm, "-mcpu=cortex-m7", "-mthumb", "-mfpu=fpv5-d16", "-mfloat-abi=hard", "-Os", "-ffunction-sections", "-fdata-sections", *STRICT, f"-I{CORE}", "-c", str(CORE / f"{name}.c"), "-o", str(obj)]
            done = subprocess.run(command, capture_output=True, text=True, check=False)
            self.assertEqual(done.returncode, 0, done.stderr)
            row = subprocess.run([size, str(obj)], capture_output=True, text=True, check=True).stdout.splitlines()[1].split()
            flash, ram = int(row[0]) + int(row[1]), int(row[2])
            self.assertLessEqual(flash, limits[name]["flash_bytes_max"], f"{name} flash {flash}")
            self.assertLessEqual(ram, limits[name]["ram_bytes_max"], f"{name} ram {ram}")
            print(f"{name} for the Cortex-M7: flash {flash} bytes, static ram {ram} bytes")


if __name__ == "__main__":
    unittest.main()
