# =============================================================================
# HYDRA-UMC - tests/test_build_firmware_port.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""tools/build_firmware.py against build_firmware.sh, stage by stage.

The shell script is cut to its first stages (the head with the variables, and the logic tests of the host), both are run in a scratch folder with a
stand-in compiler and a stand-in python3 that only write down what they were asked, and the two runs must agree: the same commands in the same order
and the same lines printed. Nothing is compiled. Needs a POSIX shell, so it is skipped elsewhere.
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


def trimmed_script(text: str) -> str:
    """The head of build_firmware.sh (its variables) and the logic tests of the host, cut at the markers of the stages."""
    lines = text.replace("\r\n", "\n").split("\n")
    first_stage = next(i for i, line in enumerate(lines) if line.startswith('step "1. Toolchain"')) - 1
    host_start = next(i for i, line in enumerate(lines) if line.startswith('step "1b.')) - 1
    host_end = next(i for i, line in enumerate(lines) if line.startswith('mkdir -p "$FIRMWARE_OUT"'))
    body = lines[:first_stage] + lines[host_start:host_end]
    return "\n".join(body) + '\necho ""; echo "$PASS passed, $WARN warnings, $FAIL failed"\n'


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

    def run_shell(self) -> tuple[list[str], list[str]]:
        root = self.work / "shell"
        root.mkdir()
        script = root / "trimmed.sh"
        script.write_text(trimmed_script((ROOT / "build_firmware.sh").read_text(encoding="utf-8")), encoding="utf-8")
        log = self.work / "shell.log"
        env = dict(self.env, FAKE_LOG=str(log))
        result = subprocess.run(["bash", str(script)], cwd=root, env=env, capture_output=True, text=True, check=False, stdin=subprocess.DEVNULL)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        ok = [line.strip() for line in result.stdout.splitlines() if line.strip().startswith("OK")]
        lines = log.read_text(encoding="utf-8").replace(str(root), "<root>").splitlines()
        return [line.replace(str(root), "<root>") for line in ok], lines

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

    def test_the_logic_tests_of_the_host_print_and_run_what_the_shell_script_does(self) -> None:
        shell_ok, shell_log = self.run_shell()
        python_ok, python_log = self.run_python()
        self.assertGreater(len(shell_ok), 80)
        self.assertEqual(python_ok, shell_ok)
        self.assertEqual(python_log, shell_log)

    @unittest.skipUnless(os.environ.get("HYDRA_BUILD_PORT_REAL") == "1" and shutil.which("cc") is not None, "set HYDRA_BUILD_PORT_REAL=1 with a C compiler")
    def test_with_the_real_compiler_both_scripts_report_the_same_tests_passing(self) -> None:
        """The workflow `build-port-check` sets the variable: the two scripts compile and run the real tests, in their own copies of the repository."""
        results = []
        for name, command in (("shell", None), ("python", [sys.executable, str(ROOT / "tools" / "build_firmware.py"), "--host-tests-only"])):
            copy = self.work / name
            shutil.copytree(ROOT, copy, ignore=shutil.ignore_patterns(".git", "build", "firmware", "__pycache__"))
            env = dict(os.environ)
            env.pop("HYDRA_UMC_SKIP_HOST_TESTS", None)
            if command is None:
                script = copy / "trimmed.sh"
                script.write_text(trimmed_script((ROOT / "build_firmware.sh").read_text(encoding="utf-8")), encoding="utf-8")
                command = ["bash", str(script)]
            else:
                command = [*command, "--root", str(copy)]
            result = subprocess.run(command, cwd=copy, env=env, capture_output=True, text=True, check=False, stdin=subprocess.DEVNULL)
            self.assertEqual(result.returncode, 0, result.stdout[-3000:] + result.stderr[-3000:])
            results.append([line.strip().replace(str(copy), "<root>") for line in result.stdout.splitlines() if line.strip().startswith("OK")])
        self.assertGreater(len(results[0]), 80)
        self.assertEqual(results[1], results[0])

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
