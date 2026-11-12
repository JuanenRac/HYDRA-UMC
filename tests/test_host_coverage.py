# =============================================================================
# HYDRA-UMC - tests/test_host_coverage.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The measuring tool of the host test coverage: its arithmetic, its reading of the gcov output, its floors file and its refusal to compile where it must not.

Nothing here compiles: the gcov text is written by hand and the run is only checked to skip where local builds are off.
"""
import contextlib
import io
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import host_coverage  # noqa: E402


def gcov_text(files, cwd):
    return json.dumps({"current_working_directory": str(cwd), "files": [{"file": name, "lines": [{"line_number": n, "count": c} for n, c in lines]} for name, lines in files]})


class Arithmetic(unittest.TestCase):
    def test_a_module_with_no_code_line_is_complete(self):
        self.assertEqual(host_coverage.percent({}), 100.0)

    def test_the_share_of_lines_that_ran(self):
        self.assertEqual(host_coverage.percent({1: 3, 2: 0, 3: 1, 4: 0}), 50.0)
        self.assertEqual(host_coverage.percent({1: 0}), 0.0)
        self.assertEqual(host_coverage.percent({1: 7, 2: 1}), 100.0)

    def test_the_runs_of_several_flavours_add_up_line_by_line(self):
        merged = host_coverage.merge_counts([{"a.h": {1: 0, 2: 1}}, {"a.h": {1: 2, 3: 0}, "b.h": {5: 1}}, {}])
        self.assertEqual(merged, {"a.h": {1: 2, 2: 1, 3: 0}, "b.h": {5: 1}})
        self.assertEqual(host_coverage.merge_counts([]), {})

    def test_a_line_that_one_flavour_leaves_out_is_not_a_gap_when_another_runs_it(self):
        merged = host_coverage.merge_counts([{"a.h": {1: 1}}, {"a.h": {1: 0, 2: 1}}])
        self.assertEqual(host_coverage.percent(merged["a.h"]), 100.0)


class Judging(unittest.TestCase):
    def test_a_module_below_its_floor_is_named_and_one_at_it_is_not(self):
        problems = host_coverage.judge({"a.h": 99.0, "b.h": 100.0}, {"a.h": 100.0, "b.h": 100.0})
        self.assertEqual(len(problems), 1)
        self.assertIn("a.h", problems[0])
        self.assertIn("99.0%", problems[0])
        self.assertEqual(host_coverage.judge({"a.h": 100.0}, {"a.h": 100.0}), [])

    def test_a_module_the_tests_never_reached_is_a_problem(self):
        problems = host_coverage.judge({}, {"a.h": 10.0})
        self.assertEqual(len(problems), 1)
        self.assertIn("no figure", problems[0])

    def test_a_module_above_its_floor_passes(self):
        self.assertEqual(host_coverage.judge({"a.h": 80.0}, {"a.h": 50.0}), [])

    def test_the_figures_are_compared_without_float_noise(self):
        self.assertEqual(host_coverage.judge({"a.h": 100.0 * 7 / 7}, {"a.h": 100.0}), [])
        self.assertEqual(host_coverage.judge({"a.h": 66.66666666666667}, {"a.h": 200.0 / 3}), [])


class Reading(unittest.TestCase):
    def test_the_files_of_the_repository_are_kept_and_the_others_left_out(self):
        inside = ROOT / "src" / "x.h"
        text = gcov_text([(str(inside), [(3, 1), (4, 0)]), ("/usr/include/stdio.h", [(1, 5)])], ROOT)
        self.assertEqual(host_coverage.parse_gcov_json(text, ROOT), {"src/x.h": {3: 1, 4: 0}})

    def test_a_relative_name_is_resolved_against_the_folder_gcov_ran_in(self):
        text = gcov_text([("../../src/y.h", [(1, 2)])], ROOT / "build" / "host_coverage")
        self.assertEqual(host_coverage.parse_gcov_json(text, ROOT), {"src/y.h": {1: 2}})

    def test_the_same_line_twice_in_one_file_adds_up(self):
        text = gcov_text([("src/z.h", [(1, 1), (1, 2), (2, 0)])], ROOT)
        self.assertEqual(host_coverage.parse_gcov_json(text, ROOT)["src/z.h"], {1: 3, 2: 0})

    def test_a_document_with_no_files_gives_nothing(self):
        self.assertEqual(host_coverage.parse_gcov_json("{}", ROOT), {})


class FloorsFile(unittest.TestCase):
    def test_every_module_of_the_floors_file_exists_and_its_floor_is_a_percentage(self):
        floors = host_coverage.module_floors()
        self.assertGreaterEqual(len(floors), 40)
        for module, floor in floors.items():
            self.assertTrue((ROOT / module).is_file(), module)
            self.assertTrue(0.0 <= floor <= 100.0, module)

    def test_the_modules_with_a_switch_are_each_held_at_the_whole(self):
        floors = host_coverage.module_floors()
        for module in ("fdcan_clock.h", "app_boot_request.h", "health_frame.h"):
            self.assertEqual(floors[f"src/mcu_stm32g474/{module}"], 100.0)
        self.assertEqual(floors["src/mcu_stm32g474/boot/adoption_guard.h"], 100.0)
        self.assertEqual(floors["src/mcu_stm32h745/CM4/rcc_owner.h"], 100.0)
        self.assertEqual(floors["src/mcu_stm32h745/CM4/spi_system.h"], 100.0)
        self.assertEqual(floors["src/mcu_stm32g474/boot/flash_layer.h"], 100.0)

    def test_a_floors_file_is_read_from_any_path(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "floors.json"
            path.write_text(json.dumps({"modules": {"a.h": 12}}), encoding="utf-8")
            self.assertEqual(host_coverage.module_floors(path), {"a.h": 12.0})


class Refusal(unittest.TestCase):
    def test_where_local_builds_are_off_the_run_says_it_skipped_and_compiles_nothing(self):
        env = {key: value for key, value in os.environ.items() if key not in ("CI", "GITHUB_ACTIONS", "ALLOW_LOCAL_BUILD")}
        out = io.StringIO()
        with mock.patch.dict(os.environ, env, clear=True), mock.patch.object(host_coverage, "measure", side_effect=AssertionError("compiled")), \
                contextlib.redirect_stdout(out):
            self.assertEqual(host_coverage.main(["--check"]), 0)
        self.assertIn("HOST_COVERAGE=SKIPPED", out.getvalue())

    def test_the_tool_covers_every_test_of_the_runner_table(self):
        names = [test.name for test in host_coverage.host_tests.table() if not test.syntax_only]
        self.assertGreaterEqual(len(names), 50)
        self.assertEqual(len(names), len(set(names)))


if __name__ == "__main__":
    unittest.main()
