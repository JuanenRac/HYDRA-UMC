# =============================================================================
# HYDRA-UMC - tests/test_build_firmware_port.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""tools/build_firmware.py with stand-in tools.

The build is run in a scratch folder with a stand-in C compiler, a stand-in python3 and stand-ins for the ARM tools and git, that only write down what they
were asked and create the files the real ones would, so nothing is compiled. The lines printed, the commands run and the files left behind are checked.
Needs a POSIX shell for the stand-ins, so it is skipped elsewhere.
"""
from __future__ import annotations

import os
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import build_firmware  # noqa: E402

FAKE_CC = """#!/bin/sh
echo "CC $*" >> "$FAKE_LOG"
out=""
prev=""
for a in "$@"; do
  if [ "$prev" = "-o" ]; then out="$a"; fi
  prev="$a"
done
if [ -n "$out" ]; then
  mkdir -p "$(dirname "$out")"
  printf '#!/bin/sh\\necho "RUN $0" >> "$FAKE_LOG"\\nexit 0\\n' > "$out"
  chmod +x "$out"
fi
exit 0
"""

FAKE_PYTHON = """#!/bin/sh
echo "PY $*" >> "$FAKE_LOG"
exit 0
"""


FAKE_ARM_GCC = """#!/bin/sh
echo "GCC $*" >> "$FAKE_LOG"
out=""
prev=""
for a in "$@"; do
  if [ "$prev" = "-o" ]; then out="$a"; fi
  prev="$a"
done
if [ -n "$out" ]; then
  mkdir -p "$(dirname "$out")"
  printf 'fake %s\\n' "$out" > "$out"
fi
exit 0
"""

FAKE_OBJCOPY = """#!/bin/sh
echo "OBJCOPY $*" >> "$FAKE_LOG"
printf 'fake objcopy\\n' > "$4"
exit 0
"""

FAKE_SIZE = """#!/bin/sh
echo "SIZE $*" >> "$FAKE_LOG"
echo "   text   data   bss   dec   hex   filename"
echo "  12345   100   200   12645   3165   $1"
exit 0
"""

FAKE_GIT = """#!/bin/sh
echo "GIT $*" >> "$FAKE_LOG"
dir=""
sub=""
while [ $# -gt 0 ]; do
  case "$1" in
    -c) shift ;;
    -C) dir="$2"; shift ;;
    -*) ;;
    *) if [ -z "$sub" ]; then sub="$1"; rest="$*"; break; fi ;;
  esac
  shift
done
mk() { mkdir -p "$(dirname "$1")"; printf 'x\\n' > "$1"; }
case "$sub" in
  clone)
    set -- $rest
    for a in "$@"; do last="$a"; done
    url=""
    for a in "$@"; do case "$a" in http*) url="$a" ;; esac; done
    d="$last"
    mkdir -p "$d"
    case "$url" in
      *stm32g4xx_hal_driver*) mk "$d/Inc/stm32g4xx_hal.h"; mk "$d/Inc/stm32g4xx_hal_conf_template.h"; mk "$d/Inc/Legacy/legacy.h"
        for m in stm32g4xx_hal stm32g4xx_hal_cortex stm32g4xx_hal_gpio stm32g4xx_hal_rcc stm32g4xx_hal_rcc_ex stm32g4xx_hal_pwr stm32g4xx_hal_pwr_ex stm32g4xx_hal_flash stm32g4xx_hal_flash_ex stm32g4xx_hal_exti stm32g4xx_hal_fdcan stm32g4xx_hal_iwdg stm32g4xx_hal_adc; do mk "$d/Src/$m.c"; done ;;
      *cmsis_device_g4*) mk "$d/Include/stm32g474xx.h"; mk "$d/Source/Templates/gcc/startup_stm32g474xx.s"; mk "$d/Source/Templates/system_stm32g4xx.c" ;;
      *stm32h7xx_hal_driver*) mk "$d/Inc/stm32h7xx_hal.h"; mk "$d/Inc/stm32h7xx_hal_conf_template.h"; mk "$d/Inc/Legacy/legacy.h"
        for m in stm32h7xx_hal stm32h7xx_hal_cortex stm32h7xx_hal_gpio stm32h7xx_hal_rcc stm32h7xx_hal_rcc_ex stm32h7xx_hal_pwr stm32h7xx_hal_pwr_ex stm32h7xx_hal_flash stm32h7xx_hal_flash_ex stm32h7xx_hal_exti stm32h7xx_hal_mdma stm32h7xx_hal_fdcan stm32h7xx_hal_iwdg stm32h7xx_hal_spi stm32h7xx_hal_hsem stm32h7xx_hal_adc; do mk "$d/Src/$m.c"; done ;;
      *cmsis_device_h7*) mk "$d/Include/stm32h757xx.h"; mk "$d/Source/Templates/gcc/startup_stm32h757xx.s"; mk "$d/Source/Templates/system_stm32h7xx_dualcore_boot_cm4_cm7.c" ;;
      *cmsis_core*) mkdir -p "$d/.git/info" ;;
      *FreeRTOS-Kernel*) for f in tasks queue list timers event_groups; do mk "$d/$f.c"; done; mk "$d/include/FreeRTOS.h"; mk "$d/portable/GCC/ARM_CM4F/port.c"; mk "$d/portable/GCC/ARM_CM7/r0p1/port.c"; mk "$d/portable/MemMang/heap_4.c" ;;
    esac ;;
  checkout) if [ -n "$dir" ]; then mk "$dir/Core/Include/core_cm4.h"; mk "$dir/Core/Include/core_cm7.h"; fi ;;
esac
exit 0
"""


@unittest.skipIf(os.name == "nt" or shutil.which("bash") is None, "needs a POSIX shell")
class BuildFirmwarePortTests(unittest.TestCase):
    def setUp(self) -> None:
        self.work = Path(tempfile.mkdtemp(prefix="hydra-build-port-"))
        self.addCleanup(shutil.rmtree, self.work, ignore_errors=True)
        self.bin = self.work / "bin"
        self.bin.mkdir()
        for name, text in (("fakecc", FAKE_CC), ("python3", FAKE_PYTHON)):
            path = self.bin / name
            path.write_text(text, encoding="utf-8")
            path.chmod(path.stat().st_mode | stat.S_IEXEC)
        self.env = dict(os.environ)
        self.env["HOST_CC"] = str(self.bin / "fakecc")
        self.env["PATH"] = f"{self.bin}{os.pathsep}{self.env['PATH']}"
        self.env.pop("HYDRA_UMC_SKIP_HOST_TESTS", None)

    def run_python(self) -> tuple[list[str], list[str]]:
        root = self.work / "python"
        root.mkdir()
        log = self.work / "python.log"
        env = dict(self.env, FAKE_LOG=str(log))
        result = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "build_firmware.py"), "--host-tests-only", "--root", str(root)],
            cwd=root,
            env=env,
            capture_output=True,
            text=True,
            check=False,
            stdin=subprocess.DEVNULL,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        ok = [line.strip() for line in result.stdout.splitlines() if line.strip().startswith("OK")]
        lines = log.read_text(encoding="utf-8").replace(str(root), "<root>").splitlines()
        return [line.replace(str(root), "<root>") for line in ok], lines

    def test_the_logic_tests_of_the_host_print_and_run_their_programs(self) -> None:
        python_ok, python_log = self.run_python()
        self.assertGreater(len(python_ok), 80)
        self.assertGreater(len(python_log), 150)

    @unittest.skipUnless(os.environ.get("HYDRA_BUILD_PORT_REAL") == "1" and shutil.which("cc") is not None, "set HYDRA_BUILD_PORT_REAL=1 with a C compiler")
    def test_with_the_real_compiler_the_logic_tests_of_the_host_pass(self) -> None:
        copy = self.work / "real"
        shutil.copytree(ROOT, copy, ignore=shutil.ignore_patterns(".git", "build", "firmware", "__pycache__"))
        env = dict(os.environ)
        env.pop("HYDRA_UMC_SKIP_HOST_TESTS", None)
        # the last step of the build runs every test of the copy, this one included: without this it would start again
        env.pop("HYDRA_BUILD_PORT_REAL", None)
        command = [sys.executable, str(ROOT / "tools" / "build_firmware.py"), "--host-tests-only", "--root", str(copy)]
        result = subprocess.run(command, cwd=copy, env=env, capture_output=True, text=True, check=False, stdin=subprocess.DEVNULL)
        self.assertEqual(result.returncode, 0, result.stdout[-3000:] + result.stderr[-3000:])
        self.assertGreater(len([line for line in result.stdout.splitlines() if line.strip().startswith("OK")]), 80)

    def test_the_compilation_of_the_boards_runs_its_commands_and_leaves_its_files(self) -> None:
        for name, text in (("arm-none-eabi-gcc", FAKE_ARM_GCC), ("arm-none-eabi-objcopy", FAKE_OBJCOPY), ("arm-none-eabi-size", FAKE_SIZE), ("git", FAKE_GIT)):
            path = self.bin / name
            path.write_text(text, encoding="utf-8")
            path.chmod(path.stat().st_mode | stat.S_IEXEC)
        for kind in ("python",):
            root = self.work / f"compile-{kind}"
            root.mkdir()
            shutil.copytree(ROOT / "src", root / "src")
            log = self.work / f"compile-{kind}.log"
            env = dict(self.env, FAKE_LOG=str(log), LC_ALL="C")
            env.pop("HYDRA_UMC_RELEASE", None)
            env.pop("HYDRA_EXTRA_CFLAGS", None)
            command = [sys.executable, str(ROOT / "tools" / "build_firmware.py"), "--compile-only", "--root", str(root)]
            result = subprocess.run(command, cwd=root, env=env, capture_output=True, text=True, check=False, stdin=subprocess.DEVNULL)
            self.assertEqual(result.returncode, 0, result.stdout[-3000:] + result.stderr[-3000:])
            reported = [line.strip().replace(str(root), "<root>") for line in result.stdout.splitlines() if line.strip().startswith(("OK", "WARN", "FAIL"))]
            commands = [line.replace(str(root), "<root>") for line in log.read_text(encoding="utf-8").splitlines()]
            files = {}
            for path in sorted((root / "build").rglob("*")):
                if path.is_file():
                    files[path.relative_to(root).as_posix()] = path.read_text(encoding="utf-8", errors="replace").replace(str(root), "<root>")
        self.assertGreater(len(reported), 35)
        self.assertGreater(len(commands), 120)
        staged = [name for name in files if name.startswith("build/firmware_staging/")]
        self.assertEqual(len(staged), 18)  # six components, each as .elf, .bin and .hex

    def test_the_whole_build_prints_its_lines_runs_its_commands_and_leaves_its_files(self) -> None:
        for name, text in (
            ("arm-none-eabi-gcc", FAKE_ARM_GCC),
            ("arm-none-eabi-objcopy", FAKE_OBJCOPY),
            ("arm-none-eabi-size", FAKE_SIZE),
            ("arm-none-eabi-nm", "#!/bin/sh\nexit 0\n"),
            ("git", FAKE_GIT),
        ):
            path = self.bin / name
            path.write_text(text, encoding="utf-8")
            path.chmod(path.stat().st_mode | stat.S_IEXEC)
        for kind in ("python",):
            root = self.work / f"whole-{kind}"
            root.mkdir()
            shutil.copytree(ROOT / "src", root / "src")
            (root / "firmware").mkdir()
            (root / "firmware" / "HYDRA_RCB_APP_v0.0.1.bin").write_text("an older build of the same component\n", encoding="utf-8")
            (root / "firmware" / "keep_me.txt").write_text("not generated\n", encoding="utf-8")
            log = self.work / f"whole-{kind}.log"
            env = dict(self.env, FAKE_LOG=str(log), LC_ALL="C", HYDRA_UMC_SKIP_HOST_TESTS="1")
            for name in ("HYDRA_UMC_RELEASE", "HYDRA_EXTRA_CFLAGS", "HYDRA_UMC_CI"):
                env.pop(name, None)
            command = [sys.executable, str(ROOT / "tools" / "build_firmware.py"), "--root", str(root)]
            result = subprocess.run(command, cwd=root, env=env, capture_output=True, text=True, check=False, stdin=subprocess.DEVNULL)
            self.assertEqual(result.returncode, 0, result.stdout[-3000:] + result.stderr[-3000:])
            lines = result.stdout.replace(str(root), "<root>").splitlines()
            start = next(i for i, line in enumerate(lines) if line.startswith("Building does not change"))
            commands = [line.replace(str(root), "<root>") for line in log.read_text(encoding="utf-8").splitlines()]
            files = {}
            for folder in ("build", "firmware"):
                for path in sorted((root / folder).rglob("*")):
                    if path.is_file():
                        files[path.relative_to(root).as_posix()] = path.read_text(encoding="utf-8", errors="replace").replace(str(root), "<root>")
        self.assertGreater(len(lines[start:]), 80)
        self.assertGreater(len(commands), 120)
        self.assertIn("Output binaries are in: <root>/firmware/", lines)
        self.assertNotIn("firmware/HYDRA_RCB_APP_v0.0.1.bin", files)  # the older version of a rebuilt component is replaced
        self.assertIn("firmware/keep_me.txt", files)  # anything else is left alone
        self.assertEqual(len([name for name in files if name.startswith("firmware/HYDRA_")]), 18)

    def test_a_failing_program_stops_the_build_with_status_one(self) -> None:
        failing = self.bin / "failcc"
        failing.write_text("#!/bin/sh\nexit 1\n", encoding="utf-8")
        failing.chmod(failing.stat().st_mode | stat.S_IEXEC)
        root = self.work / "failing"
        root.mkdir()
        env = dict(self.env, HOST_CC=str(failing))
        result = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "build_firmware.py"), "--host-tests-only", "--root", str(root)],
            cwd=root,
            env=env,
            capture_output=True,
            text=True,
            check=False,
            stdin=subprocess.DEVNULL,
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("FAIL host test suite failed to compile", result.stdout)
        self.assertIn("0 passed, 0 warnings, 1 failed", result.stdout)

    def test_the_tests_of_the_host_can_be_skipped_and_say_so(self) -> None:
        root = self.work / "skipped"
        root.mkdir()
        env = dict(self.env, HYDRA_UMC_SKIP_HOST_TESTS="1")
        result = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "build_firmware.py"), "--host-tests-only", "--root", str(root)],
            cwd=root,
            env=env,
            capture_output=True,
            text=True,
            check=False,
            stdin=subprocess.DEVNULL,
        )
        self.assertEqual(result.returncode, 0)
        self.assertIn("WARN host logic tests skipped (HYDRA_UMC_SKIP_HOST_TESTS=1)", result.stdout)


class TableTests(unittest.TestCase):
    """What can be checked without a shell."""

    def test_every_case_names_a_test_source_that_exists(self) -> None:
        cases = build_firmware.host_cases("<root>") + build_firmware.bootloader_cases("<root>")
        self.assertGreater(len(cases), 60)
        for case in cases:
            for compile_ in case.compiles:
                self.assertTrue((ROOT / "tests" / f"{compile_.test}.c").is_file(), compile_.test)
                for include in compile_.includes:
                    self.assertTrue((ROOT / include).is_dir(), include)
                for source in compile_.sources:
                    if source != build_firmware.TWEETNACL:
                        self.assertTrue((ROOT / source).is_file(), source)

    def test_the_summary_counts_what_was_reported(self) -> None:
        lines: list[str] = []
        report = build_firmware.Report(out=lines.append)
        report.ok("a")
        report.warn("b")
        report.fail("c")
        report.summary()
        self.assertEqual(lines[-1], "1 passed, 1 warnings, 1 failed")

    def test_a_program_that_cannot_be_started_is_a_failure_not_a_crash(self) -> None:
        self.assertEqual(build_firmware.default_runner(["/nonexistent/program-for-the-test"], True), 127)


if __name__ == "__main__":
    unittest.main()
