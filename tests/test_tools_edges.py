# =============================================================================
# HYDRA-UMC - tests/test_tools_edges.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Edges of the small build tools: the host test table and runner, the machine configuration commands and the changelog section extractor.

Nothing here compiles or flashes anything: the compiler calls are replaced, and every file is written in a throw-away folder.
"""
import configparser
import contextlib
import io
import os
import random
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import changelog_section  # noqa: E402
import host_tests  # noqa: E402
import machine_cfg  # noqa: E402

BASE = (ROOT / "config" / "machine.cfg").read_text(encoding="utf-8")


def capture(function, *args):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = function(*args)
    return code, out.getvalue(), err.getvalue()


class HostTestTableTests(unittest.TestCase):
    def test_every_test_has_a_unique_name_and_an_existing_source(self):
        names = [test.name for test in host_tests.table()]
        self.assertEqual(len(names), len(set(names)))
        for test in host_tests.table():
            self.assertTrue((ROOT / test.source).is_file(), f"{test.name}: {test.source}")
            for extra in test.extra_sources:
                self.assertTrue((ROOT / extra).is_file(), f"{test.name}: {extra}")

    def test_every_include_folder_of_every_test_exists(self):
        for test in host_tests.table():
            for include in test.includes:
                self.assertTrue((ROOT / include).is_dir(), f"{test.name}: {include}")

    def test_every_c_test_file_is_in_the_table_or_is_a_shared_helper(self):
        used = {test.source for test in host_tests.table()}
        helpers = {"tests/relay_golden_dump.c"}
        for path in (ROOT / "tests").glob("*.c"):
            relative = path.relative_to(ROOT).as_posix()
            self.assertTrue(relative in used or relative in helpers, f"{relative} is not run by tools/host_tests.py")

    def test_the_boot_tests_run_on_all_three_bootloaders(self):
        names = {test.name for test in host_tests.table()}
        for directory in host_tests.BOOT_DIRS:
            for kind in ("boot_decision", "test_image_header", "test_update_verify", "trust", "trust_release"):
                self.assertIn(f"{kind}@{directory}", names)

    def test_the_image_and_update_tests_are_built_with_warnings_as_errors(self):
        for test in host_tests.table():
            if test.name.startswith(("test_image_header@", "test_update_verify@")):
                self.assertIn("-Werror", test.flags, test.name)

    def test_only_the_protocol_units_are_syntax_only_and_they_need_no_binary(self):
        syntax = [test.name for test in host_tests.table() if test.syntax_only]
        self.assertEqual(sorted(syntax), ["bootloader_h745_CM4_protocol", "bootloader_h745_CM7_protocol"])

    def test_the_tests_that_need_the_signature_library_say_so(self):
        for test in host_tests.table():
            if "bootloader_trust.c" in " ".join(test.extra_sources) or test.name.startswith("bootloader_g474"):
                self.assertTrue(test.needs_tweetnacl, test.name)

    def test_safe_names_keep_letters_and_digits_only(self):
        self.assertEqual(host_tests._safe("boot_decision@src/mcu_stm32g474/boot"), "boot_decision_src_mcu_stm32g474_boot")
        self.assertEqual(host_tests._safe(""), "")
        self.assertEqual(host_tests._safe("a b-c.d"), "a_b_c_d")


class BuildCommandTests(unittest.TestCase):
    def test_a_normal_test_compiles_the_source_into_the_given_binary_with_its_flags_defines_and_includes(self):
        test = host_tests.HostTest("t", "tests/test_x.c", ("tests", "src"), host_tests.GNU, ("a/b.c",), ("ONE", "TWO=2"))
        command = host_tests.build_command("cc", test, Path("out/t"), None)
        self.assertEqual(command[0], "cc")
        self.assertEqual(command[1:1 + len(host_tests.GNU)], list(host_tests.GNU))
        self.assertIn("-DONE", command)
        self.assertIn("-DTWO=2", command)
        self.assertIn(f"-I{ROOT / 'tests'}", command)
        self.assertIn(f"-I{ROOT / 'src'}", command)
        self.assertEqual(command[command.index("-o") + 1], str(Path("out/t")))
        self.assertIn(str(ROOT / "tests/test_x.c"), command)
        self.assertEqual(command[-1], str(ROOT / "a/b.c"), "extra sources come last")

    def test_the_signature_object_is_linked_only_for_a_test_that_needs_it_and_only_when_there_is_one(self):
        needing = host_tests.HostTest("n", "tests/test_x.c", (), needs_tweetnacl=True)
        plain = host_tests.HostTest("p", "tests/test_x.c", ())
        library = Path("build/tweetnacl.o")
        self.assertIn(str(library), host_tests.build_command("cc", needing, Path("o"), library))
        self.assertNotIn(str(library), host_tests.build_command("cc", plain, Path("o"), library))
        self.assertNotIn("None", " ".join(host_tests.build_command("cc", needing, Path("o"), None)))

    def test_a_syntax_only_test_never_names_an_output_file(self):
        test = host_tests.HostTest("s", "tests/test_x.c", ("tests",), syntax_only=True, extra_sources=("never.c",))
        command = host_tests.build_command("clang", test, Path("o"), Path("t.o"))
        self.assertIn("-fsyntax-only", command)
        self.assertNotIn("-o", command)
        self.assertNotIn(str(ROOT / "never.c"), command)
        self.assertEqual(command[-1], str(ROOT / "tests/test_x.c"))


class RunAllTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        patcher = mock.patch.object(host_tests, "OUT", Path(self.tmp.name) / "out")
        patcher.start()
        self.addCleanup(patcher.stop)

    def run_with(self, compile_codes=lambda cmd: 0, run_code=lambda cmd: 0):
        calls = []

        def fake(command, **kwargs):
            calls.append(list(command))
            is_compile = command[0] == "cc"
            code = compile_codes(command) if is_compile else run_code(command)
            return mock.Mock(returncode=code)
        with mock.patch.object(host_tests.subprocess, "run", side_effect=fake):
            result = capture(host_tests.run_all, "cc")
        return result, calls

    def test_all_pass_prints_one_line_per_test_and_the_total(self):
        (code, out, _), calls = self.run_with()
        self.assertEqual(code, 0)
        lines = out.strip().splitlines()
        self.assertEqual(lines[-1], "HOST_TESTS=PASS")
        self.assertEqual(len(lines) - 1, len(host_tests.table()))
        self.assertTrue(all(line.startswith("PASS ") for line in lines[:-1]))
        syntax = [line for line in lines if line.endswith("(syntax)")]
        self.assertEqual(len(syntax), 2)
        # the library once, then one compile per test, and a run for every test that is not syntax only
        runs = len(host_tests.table()) - 2
        self.assertEqual(len(calls), 1 + len(host_tests.table()) + runs)

    def test_a_test_that_does_not_compile_is_a_failure_and_is_not_run(self):
        (code, out, _), calls = self.run_with(compile_codes=lambda cmd: 1 if any("relay_rx_queue" in part for part in cmd) else 0)
        self.assertEqual(code, 1)
        self.assertIn("FAIL relay_rx_queue: does not compile\n", out)
        self.assertEqual(out.strip().splitlines()[-1], "HOST_TESTS=FAIL (1)")
        self.assertFalse(any(call[0].endswith("relay_rx_queue") for call in calls), "a binary that was not built is not run")

    def test_a_test_that_runs_and_fails_is_counted_once(self):
        (code, out, _), _ = self.run_with(run_code=lambda cmd: 1 if "relay_rx_queue" in cmd[0] else 0)
        self.assertEqual(code, 1)
        self.assertIn("FAIL relay_rx_queue\n", out)
        self.assertEqual(out.strip().splitlines()[-1], "HOST_TESTS=FAIL (1)")

    def test_when_the_signature_library_does_not_compile_that_is_reported_and_counts(self):
        (code, out, _), _ = self.run_with(compile_codes=lambda cmd: 1 if "tweetnacl.c" in " ".join(cmd) and "-c" in cmd else 0)
        self.assertEqual(code, 1)
        self.assertIn("FAIL tweetnacl does not compile", out)
        self.assertEqual(out.strip().splitlines()[-1], "HOST_TESTS=FAIL (1)")

    def test_the_output_folder_is_created(self):
        self.run_with()
        self.assertTrue(host_tests.OUT.is_dir())

    def test_the_executable_suffix_follows_the_system(self):
        with mock.patch.object(host_tests.os, "name", "nt"):
            _, calls = self.run_with()
        self.assertTrue(any(call[0].endswith(".exe") for call in calls if call[0] != "cc"))


class HostTestsMainTests(unittest.TestCase):
    def test_list_prints_every_test_and_builds_nothing(self):
        with mock.patch.object(host_tests.subprocess, "run", side_effect=AssertionError("compiled")):
            code, out, _ = capture(host_tests.main, ["--list"])
        self.assertEqual(code, 0)
        self.assertEqual(len(out.strip().splitlines()), len(host_tests.table()))
        self.assertIn("relay_rx_queue tests/test_relay_rx_queue.c", out)

    def test_without_a_compiler_the_run_is_skipped_and_only_require_makes_it_fail(self):
        with mock.patch.object(host_tests, "find_compiler", return_value=None), mock.patch.object(host_tests, "local_build_refusal", return_value=None):
            code, out, _ = capture(host_tests.main, [])
            self.assertEqual(code, 0)
            self.assertIn("HOST_TESTS=SKIPPED no host C compiler", out)
            code, out, _ = capture(host_tests.main, ["--require"])
            self.assertEqual(code, 1)
            self.assertIn("HOST_TESTS=SKIPPED", out)

    def test_the_lock_on_local_builds_is_the_reason_given_when_it_is_the_cause(self):
        with mock.patch.object(host_tests, "find_compiler", return_value=None), mock.patch.object(host_tests, "local_build_refusal", return_value="locked here"):
            code, out, _ = capture(host_tests.main, ["--require"])
        self.assertEqual(code, 1)
        self.assertIn("HOST_TESTS=SKIPPED locked here", out)

    def test_a_compiler_runs_the_whole_table(self):
        with mock.patch.object(host_tests, "find_compiler", return_value="cc"), mock.patch.object(host_tests, "run_all", return_value=0) as run_all:
            self.assertEqual(host_tests.main([]), 0)
        run_all.assert_called_once_with("cc")

    def test_the_compiler_comes_from_host_cc_first_then_cc_gcc_clang(self):
        seen = []

        def which(name):
            seen.append(name)
            return f"/bin/{name}" if name == "gcc" else None
        env = {"CI": "true", "HOST_CC": "mycc"}
        with mock.patch.dict(os.environ, env), mock.patch.object(host_tests.shutil, "which", side_effect=which):
            self.assertEqual(host_tests.find_compiler(), "/bin/gcc")
        self.assertEqual(seen, ["mycc", "cc", "gcc"])

    def test_no_compiler_on_the_path_is_none(self):
        with mock.patch.dict(os.environ, {"CI": "true", "HOST_CC": ""}), mock.patch.object(host_tests.shutil, "which", return_value=None):
            self.assertIsNone(host_tests.find_compiler())


class MachineConfigEdgeTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)

    def load(self, text):
        path = self.dir / "machine.cfg"
        path.write_text(text, encoding="utf-8")
        return machine_cfg.load(path)

    def test_a_missing_section_is_named(self):
        for section in ("stop", "estop_input", "production"):
            parser = configparser.ConfigParser(inline_comment_prefixes=None)
            parser.read_string(BASE)
            parser.remove_section(section)
            out = io.StringIO()
            parser.write(out)
            with self.subTest(section), self.assertRaisesRegex(machine_cfg.ConfigError, rf"\[{section}\] is missing"):
                self.load(out.getvalue())

    def test_a_key_without_a_default_that_is_missing_or_empty_is_named(self):
        for key in ("ramp_max_ms", "enabled", "pin", "healthy_level", "trip_debounce_ms", "clear_debounce_ms", "trip_repeat_ms", "rdp_level", "default_category"):
            lines = [line for line in BASE.splitlines() if not line.startswith(f"{key} ")]
            with self.subTest(f"{key} removed"), self.assertRaisesRegex(machine_cfg.ConfigError, f"{key} is missing"):
                self.load("\n".join(lines))
            blanked = [f"{key} =" if line.startswith(f"{key} ") else line for line in BASE.splitlines()]
            with self.subTest(f"{key} empty"), self.assertRaisesRegex(machine_cfg.ConfigError, f"{key} is missing"):
                self.load("\n".join(blanked))

    def test_a_value_that_is_not_a_whole_number_is_quoted_back(self):
        for bad in ("fast", "1.5", "0x10", "2 ms", "--3"):
            text = BASE.replace("ramp_max_ms = 200", f"ramp_max_ms = {bad}")
            with self.subTest(bad), self.assertRaisesRegex(machine_cfg.ConfigError, "whole number"):
                self.load(text)

    def test_every_numeric_key_has_its_edges_checked(self):
        limits = {
            "ramp_max_ms": (1, 2000), "pin": (0, 15), "trip_debounce_ms": (0, 1000), "clear_debounce_ms": (0, 5000), "trip_repeat_ms": (1, 1000),
            "default_category": (0, 1), "healthy_level": (0, 1), "rdp_level": (0, 1), "enabled": (0, 1),
        }
        for key, (low, high) in limits.items():
            for value in (low - 1, high + 1):
                text = "\n".join(f"{key} = {value}" if line.startswith(f"{key} ") else line for line in BASE.splitlines())
                with self.subTest(f"{key}={value}"), self.assertRaises(machine_cfg.ConfigError):
                    self.load(text)

    def test_the_inclusive_edges_are_accepted(self):
        text = BASE.replace("ramp_max_ms = 200", "ramp_max_ms = 2000").replace("trip_debounce_ms = 2", "trip_debounce_ms = 0").replace("clear_debounce_ms = 50", "clear_debounce_ms = 5000")
        cfg = self.load(text)
        self.assertEqual((cfg["stop_ramp_max_ms"], cfg["input_trip_debounce_ms"], cfg["input_clear_debounce_ms"]), (2000, 0, 5000))

    def test_the_input_pin_must_be_one_of_the_free_pins_while_the_input_is_enabled(self):
        for port, pin in sorted(machine_cfg.FREE_PINS):
            text = BASE.replace("port = C", f"port = {port.lower()}").replace("pin = 9", f"pin = {pin}")
            self.assertEqual(self.load(text)["input_port"], port, "the port is read in any case")
        with self.assertRaisesRegex(machine_cfg.ConfigError, "free pins"):
            self.load(BASE.replace("pin = 9", "pin = 8"))
        with self.assertRaisesRegex(machine_cfg.ConfigError, "free pins"):
            self.load(BASE.replace("port = C", "port = D"))
        # switched off, the pin is not looked at
        self.assertEqual(self.load(BASE.replace("enabled = 1", "enabled = 0").replace("pin = 9", "pin = 8"))["input_enabled"], 0)

    def test_the_mask_has_one_bit_per_axis_in_axis_order(self):
        rng = random.Random(99)
        for _ in range(12):
            bits = [rng.randrange(2) for _ in range(6)]
            text = BASE
            for index, bit in enumerate(bits, start=1):
                text = text.replace(f"axis{index}_category =", f"axis{index}_category = {bit}")
            expected = sum(bit << index for index, bit in enumerate(bits))
            cfg = self.load(text)
            self.assertEqual(cfg["stop_categories"], bits)
            self.assertIn(f"#define MACHINE_STOP_CATEGORY1_MASK 0x{expected:02X}u", machine_cfg.render(cfg))

    def test_the_rendered_header_is_stable_ends_with_a_newline_and_carries_every_value(self):
        cfg = machine_cfg.load()
        text = machine_cfg.render(cfg)
        self.assertEqual(text, machine_cfg.render(cfg))
        self.assertTrue(text.endswith("#endif /* MACHINE_CONFIG_H */\n"))
        self.assertNotIn("\r", text)
        for needle in (f"MACHINE_STOP_RAMP_MAX_MS {cfg['stop_ramp_max_ms']}u", f"MACHINE_ESTOP_INPUT_PIN {cfg['input_pin']}u", f"MACHINE_PRODUCTION_RDP_LEVEL {cfg['rdp_level']}u",
                       f"MACHINE_ESTOP_INPUT_PORT '{cfg['input_port']}'"):
            self.assertIn(needle, text)

    def test_generate_writes_the_header_and_check_then_passes(self):
        header = self.dir / "machine_config.h"
        with mock.patch.object(machine_cfg, "HEADER_PATH", header), mock.patch.object(machine_cfg, "ROOT", self.dir):
            code, out, _ = capture(machine_cfg.main, ["machine_cfg.py", "generate"])
            self.assertEqual(code, 0)
            self.assertIn("wrote machine_config.h", out)
            self.assertEqual(header.read_text(encoding="utf-8"), machine_cfg.render(machine_cfg.load()))
            self.assertNotIn(b"\r", header.read_bytes())
            code, out, _ = capture(machine_cfg.main, ["machine_cfg.py", "check"])
            self.assertEqual((code, out.strip()), (0, "machine_config.h matches config/machine.cfg"))
            code, out, _ = capture(machine_cfg.main, ["machine_cfg.py"])
            self.assertEqual(code, 0, "check is the default command")

    def test_check_fails_for_a_missing_or_stale_header(self):
        header = self.dir / "machine_config.h"
        with mock.patch.object(machine_cfg, "HEADER_PATH", header):
            code, out, _ = capture(machine_cfg.main, ["machine_cfg.py", "check"])
            self.assertEqual(code, 1)
            self.assertIn("stale", out)
            header.write_text("// edited by hand\n", encoding="utf-8")
            self.assertEqual(capture(machine_cfg.main, ["machine_cfg.py", "check"])[0], 1)

    def test_a_bad_file_is_reported_and_nothing_is_written(self):
        header = self.dir / "machine_config.h"
        with mock.patch.object(machine_cfg, "HEADER_PATH", header), mock.patch.object(machine_cfg, "load", side_effect=machine_cfg.ConfigError("ramp_max_ms is missing")):
            code, out, _ = capture(machine_cfg.main, ["machine_cfg.py", "generate"])
        self.assertEqual(code, 1)
        self.assertEqual(out.strip(), "machine.cfg: ramp_max_ms is missing")
        self.assertFalse(header.exists())

    def test_an_unknown_command_prints_the_help_and_exits_2(self):
        code, out, _ = capture(machine_cfg.main, ["machine_cfg.py", "bogus"])
        self.assertEqual(code, 2)
        self.assertIn("machine_cfg.py generate", out)


CHANGELOG = """# Changelog

## [Unreleased]

### Added
- something new

## [1.2.3] - the third
### Fixed
- a fix

- another line

## [1.2.30]
- not the one

## [1.2.4]

## [1.2.5] - empty above
- last
"""


class ChangelogSectionTests(unittest.TestCase):
    def test_the_body_of_a_section_stops_at_the_next_heading(self):
        text = changelog_section.section(CHANGELOG, "1.2.3")
        self.assertEqual(text, "### Fixed\n- a fix\n\n- another line\n")

    def test_the_heading_can_be_kept(self):
        text = changelog_section.section(CHANGELOG, "1.2.3", True)
        self.assertTrue(text.startswith("## [1.2.3] - the third\n\n### Fixed"))

    def test_a_version_is_matched_whole_not_as_a_prefix(self):
        self.assertEqual(changelog_section.section(CHANGELOG, "1.2.30"), "- not the one\n")
        self.assertIsNone(changelog_section.section(CHANGELOG, "1.2"))
        self.assertIsNone(changelog_section.section(CHANGELOG, "1.2.300"))

    def test_the_dots_of_a_version_are_not_wildcards(self):
        self.assertIsNone(changelog_section.section("## [1x2x3]\n- body\n", "1.2.3"))

    def test_an_empty_section_and_an_absent_one_are_none(self):
        self.assertIsNone(changelog_section.section(CHANGELOG, "1.2.4"))
        self.assertIsNone(changelog_section.section(CHANGELOG, "9.9.9"))
        self.assertIsNone(changelog_section.section("", "1.0.0"))

    def test_the_last_section_runs_to_the_end_of_the_file(self):
        self.assertEqual(changelog_section.section(CHANGELOG, "1.2.5"), "- last\n")

    def test_windows_line_ends_are_read_like_unix_ones(self):
        self.assertEqual(changelog_section.section(CHANGELOG.replace("\n", "\r\n"), "1.2.3"), changelog_section.section(CHANGELOG, "1.2.3"))

    def test_the_heading_may_end_right_after_the_bracket(self):
        self.assertEqual(changelog_section.section("## [2.0.0]\n- x\n", "2.0.0"), "- x\n")

    def test_main_prints_the_section_and_fails_for_an_absent_one_or_a_bad_version(self):
        with tempfile.TemporaryDirectory() as folder:
            (Path(folder) / "CHANGELOG.md").write_text(CHANGELOG, encoding="utf-8")
            with mock.patch.object(changelog_section, "ROOT", Path(folder)):
                code, out, err = capture(changelog_section.main, ["1.2.3"])
                self.assertEqual((code, err), (0, ""))
                self.assertTrue(out.startswith("### Fixed"))
                code, out, err = capture(changelog_section.main, ["1.2.3", "--heading"])
                self.assertTrue(out.startswith("## [1.2.3]"))
                code, out, err = capture(changelog_section.main, ["7.7.7"])
                self.assertEqual((code, out), (1, ""))
                self.assertIn("no section for 7.7.7", err)
                self.assertEqual(capture(changelog_section.main, ["1.2.4"])[0], 1, "an empty section fails so no release goes out without notes")
                for bad in ([], ["1.2"], ["v1.2.3"], ["1.2.3", "1.2.4"], ["1.2.3.4.5"], ["a.b.c"]):
                    code, out, err = capture(changelog_section.main, bad)
                    self.assertEqual(code, 2, bad)
                    self.assertIn("usage:", err)
                self.assertEqual(capture(changelog_section.main, ["0.8.0.1"])[0], 1, "four components are accepted as a version")


if __name__ == "__main__":
    unittest.main()
