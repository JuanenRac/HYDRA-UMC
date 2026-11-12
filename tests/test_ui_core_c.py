# =============================================================================
# HYDRA-UMC - tests/test_ui_core_c.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The view-model in C (src/ui_h757/core/ui_core.c) is built for the PC with the compiler that is there and replays every scenario against the trace of the Python reference
(src/ui_h757/test/ui_core_test.c). With no host compiler, or on a PC where compiling is locked (tools/local_build.py), the test says so and skips: the CI runs it. When the ARM
compiler is there, the same sources are also built for the Cortex-M7 and the size of the core is held to the budget (src/ui_h757/budget.json). Evidence: simulator."""
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

CORE = ("src/ui_h757/core/ui_core.c",)
STRICT = ("-std=c11", "-Wall", "-Wextra", "-Werror")


class CoreInCTests(unittest.TestCase):
    def setUp(self):
        self.compiler = host_tests.find_compiler()
        if self.compiler is None:
            self.skipTest("no host C compiler (or compiling on this PC is locked): the CI runs this test")
        self.work = Path(tempfile.mkdtemp(prefix="ui_core_c_"))
        self.addCleanup(shutil.rmtree, self.work, True)

    def build(self, *extra):
        exe = self.work / ("ui_core_test.exe" if sys.platform == "win32" else "ui_core_test")
        command = [self.compiler, *STRICT, "-O1", f"-I{ROOT / 'src' / 'ui_h757' / 'core'}", f"-I{ROOT / 'src' / 'ui_h757' / 'test'}", *extra, "-o", str(exe),
                   str(ROOT / "src" / "ui_h757" / "test" / "ui_core_test.c"), *[str(ROOT / source) for source in CORE]]
        done = subprocess.run(command, capture_output=True, text=True, check=False)
        self.assertEqual(done.returncode, 0, done.stderr)
        return exe

    def test_the_core_replays_every_scenario_like_the_reference_and_holds_the_rules(self):
        exe = self.build()
        done = subprocess.run([str(exe)], capture_output=True, text=True, check=False)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertIn("ui_core: PASS", done.stdout)
        scenarios = len(json.loads((ROOT / "src" / "ui_h757" / "spec" / "scenarios.json").read_text(encoding="utf-8"))["scenarios"])
        self.assertIn(f"{scenarios} scenarios replayed in C", done.stdout)

    def test_the_sanitizers_find_nothing(self):
        exe = self.build("-fsanitize=address,undefined", "-fno-sanitize-recover=undefined")
        done = subprocess.run([str(exe)], capture_output=True, text=True, check=False)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)


class CoreForTheBoardTests(unittest.TestCase):
    def test_the_core_builds_for_the_cortex_m7_and_fits_its_budget(self):
        arm = shutil.which("arm-none-eabi-gcc")
        size = shutil.which("arm-none-eabi-size")
        if arm is None or size is None:
            self.skipTest("the ARM compiler is not on this PC: the CI job that has it measures the core")
        work = Path(tempfile.mkdtemp(prefix="ui_core_arm_"))
        self.addCleanup(shutil.rmtree, work, True)
        obj = work / "ui_core.o"
        command = [arm, "-mcpu=cortex-m7", "-mthumb", "-mfpu=fpv5-d16", "-mfloat-abi=hard", "-Os", "-ffunction-sections", "-fdata-sections", "-std=c11", "-Wall", "-Wextra",
                   "-Werror", f"-I{ROOT / 'src' / 'ui_h757' / 'core'}", "-c", str(ROOT / "src" / "ui_h757" / "core" / "ui_core.c"), "-o", str(obj)]
        done = subprocess.run(command, capture_output=True, text=True, check=False)
        self.assertEqual(done.returncode, 0, done.stderr)
        out = subprocess.run([size, str(obj)], capture_output=True, text=True, check=True).stdout.splitlines()[1].split()
        text_bytes, data_bytes, bss_bytes = int(out[0]), int(out[1]), int(out[2])
        budget = json.loads((ROOT / "src" / "ui_h757" / "budget.json").read_text(encoding="utf-8"))["measured_limits"]["ui_core"]
        self.assertLessEqual(text_bytes + data_bytes, budget["flash_bytes_max"], f"core flash {text_bytes + data_bytes}")
        self.assertLessEqual(bss_bytes, budget["ram_bytes_max"], f"core ram {bss_bytes}")
        print(f"ui_core for the Cortex-M7: flash {text_bytes + data_bytes} bytes, static ram {bss_bytes} bytes")


if __name__ == "__main__":
    unittest.main()
