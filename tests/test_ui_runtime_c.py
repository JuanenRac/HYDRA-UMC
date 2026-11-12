# =============================================================================
# HYDRA-UMC - tests/test_ui_runtime_c.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The runtime of the interface (src/ui_h757/core/ui_runtime.c) is built for the PC and run against fakes of the Jetson link, of the F-RAM of the brain (the real driver over a fake
I2C bus) and of the machine, with the sanitizers; and built for the Cortex-M7, where its size is held to its limit in src/ui_h757/budget.json. With no host compiler, or where compiling
on this PC is locked (tools/local_build.py), the host tests skip and the CI runs them. Evidence: simulator."""
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import host_tests  # noqa: E402

CORE = ROOT / "src" / "ui_h757" / "core"
TEST = ROOT / "src" / "ui_h757" / "test"
CM4 = ROOT / "src" / "mcu_stm32h757" / "CM4"
SOURCES = [TEST / "ui_runtime_test.c", CORE / "ui_runtime.c", CORE / "ui_core.c", CORE / "ui_link.c", CORE / "ui_pin.c"]
STRICT = ("-std=c11", "-Wall", "-Wextra", "-Werror")


class HostTests(unittest.TestCase):
    def setUp(self):
        self.compiler = host_tests.find_compiler()
        if self.compiler is None:
            self.skipTest("no host C compiler (or compiling on this PC is locked): the CI runs this test")
        self.work = Path(tempfile.mkdtemp(prefix="ui_runtime_c_"))
        self.addCleanup(shutil.rmtree, self.work, True)

    def run_test(self, *extra):
        exe = self.work / ("ui_runtime_test.exe" if sys.platform == "win32" else "ui_runtime_test")
        command = [self.compiler, *STRICT, "-O1", f"-I{CORE}", f"-I{CM4}", *extra, "-o", str(exe), *[str(s) for s in SOURCES]]
        done = subprocess.run(command, capture_output=True, text=True, check=False)
        self.assertEqual(done.returncode, 0, done.stderr)
        run = subprocess.run([str(exe)], capture_output=True, text=True, check=False)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        return run.stdout

    def test_the_runtime_chain_works_against_the_fakes(self):
        self.assertIn("ui_runtime: PASS", self.run_test())

    def test_the_runtime_chain_is_clean_under_the_sanitizers(self):
        self.assertIn("ui_runtime: PASS", self.run_test("-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"))


class ForTheBoardTests(unittest.TestCase):
    def test_the_runtime_builds_for_the_cortex_m7_and_fits_its_limit(self):
        arm, size = shutil.which("arm-none-eabi-gcc"), shutil.which("arm-none-eabi-size")
        if arm is None or size is None:
            self.skipTest("the ARM compiler is not on this PC: the CI job that has it measures the runtime")
        limits = json.loads((ROOT / "src" / "ui_h757" / "budget.json").read_text(encoding="utf-8"))["measured_limits"]["ui_runtime"]
        work = Path(tempfile.mkdtemp(prefix="ui_runtime_arm_"))
        self.addCleanup(shutil.rmtree, work, True)
        obj = work / "ui_runtime.o"
        command = [arm, "-mcpu=cortex-m7", "-mthumb", "-mfpu=fpv5-d16", "-mfloat-abi=hard", "-Os", *STRICT, f"-I{CORE}", "-c", str(CORE / "ui_runtime.c"), "-o", str(obj)]
        done = subprocess.run(command, capture_output=True, text=True, check=False)
        self.assertEqual(done.returncode, 0, done.stderr)
        row = subprocess.run([size, str(obj)], capture_output=True, text=True, check=True).stdout.splitlines()[1].split()
        flash, ram = int(row[0]) + int(row[1]), int(row[2])
        self.assertLessEqual(flash, limits["flash_bytes_max"], f"flash {flash}")
        self.assertLessEqual(ram, limits["ram_bytes_max"], f"ram {ram}")
        print(f"ui_runtime for the Cortex-M7: flash {flash} bytes, static ram {ram} bytes")


if __name__ == "__main__":
    unittest.main()
