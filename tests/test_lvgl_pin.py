# =============================================================================
# HYDRA-UMC - tests/test_lvgl_pin.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The interface library pinned by commit: the pin of config/lvgl.cfg is an exact commit of a 9.x release of the project and no file of the library is in the repository, the fetch
refuses a folder inside the repository and a commit that is not the pin, the compile takes the library only from outside, the sizes are read from the size tool and held to the flash
budget, and the workflow that does it in Actions only reads. Evidence: simulator (the compile itself is the workflow's)."""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import lvgl_pin as lp  # noqa: E402

WORKFLOW = (ROOT / ".github/workflows/lvgl-library.yml").read_text(encoding="utf-8").replace("\r\n", "\n")
GOOD = {"repository": lp.REPOSITORY, "tag": "v9.6.0", "commit": "a" * 40, "license": "MIT", "flash_budget_bytes": 307200}


class Pin(unittest.TestCase):
    def test_the_file_is_an_exact_commit_of_a_9x_release(self):
        cfg = lp.load_config()
        self.assertRegex(cfg["commit"], r"^[0-9a-f]{40}$")
        self.assertRegex(cfg["tag"], r"^v9\.\d+\.\d+$")
        self.assertEqual(cfg["license"], "MIT")

    def test_the_budget_is_the_flash_of_the_interface_budget(self):
        self.assertEqual(lp.load_config()["flash_budget_bytes"], 300 * 1024)

    def test_no_file_of_the_library_is_in_the_repository(self):
        self.assertEqual(lp.vendored_files(), [])
        with tempfile.TemporaryDirectory() as folder:
            (Path(folder) / "third_party").mkdir()
            (Path(folder) / "third_party" / "lvgl.h").write_text("x", encoding="utf-8")
            (Path(folder) / "build").mkdir()
            (Path(folder) / "build" / "lv_obj.c").write_text("x", encoding="utf-8")
            self.assertEqual(lp.vendored_files(Path(folder)), ["third_party/lvgl.h"])

    def test_every_bad_pin_is_refused(self):
        lp.check(GOOD)
        for key, value in (
            ("repository", "https://example.com/lvgl"),
            ("commit", "v9.6.0"),
            ("commit", "a" * 39),
            ("commit", "A" * 40),
            ("commit", "master"),
            ("tag", "v8.4.0"),
            ("tag", "latest"),
            ("license", "GPL"),
            ("flash_budget_bytes", 100),
            ("flash_budget_bytes", 10**7),
        ):
            with self.assertRaises(ValueError, msg=f"{key}={value}"):
                lp.check(dict(GOOD, **{key: value}))

    def test_a_bad_file_is_refused(self):
        base = (ROOT / "config/lvgl.cfg").read_text(encoding="utf-8").replace("\r\n", "\n")
        cfg = lp.load_config()
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "lvgl.cfg"
            path.write_text(base.replace(cfg["commit"], cfg["tag"], 1), encoding="utf-8")
            with self.assertRaises(ValueError):
                lp.load_config(path)

    def test_the_library_is_not_in_the_budget_file_as_a_loose_version(self):
        text = (ROOT / "src/ui_h757/budget.json").read_text(encoding="utf-8")
        self.assertIn("pinned by commit", text)


class Fetch(unittest.TestCase):
    def fake(self, head, calls):
        def run(args, check=True, capture_output=False, text=False):
            calls.append(args)
            return SimpleNamespace(stdout=head + "\n")

        return run

    def test_it_fetches_the_exact_commit_into_an_empty_folder_outside(self):
        calls = []
        with tempfile.TemporaryDirectory() as folder:
            dest = Path(folder) / "lvgl"
            self.assertEqual(lp.fetch(GOOD, dest, run=self.fake(GOOD["commit"], calls)), GOOD["commit"])
            self.assertTrue(dest.is_dir())
        fetched = [c for c in calls if "fetch" in c][0]
        self.assertEqual(fetched[-1], GOOD["commit"])
        self.assertIn("--depth", fetched)
        self.assertEqual([c[3] for c in calls], ["init", "fetch", "checkout", "rev-parse"])

    def test_a_commit_that_is_not_the_pin_is_refused(self):
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaises(RuntimeError):
                lp.fetch(GOOD, Path(folder) / "lvgl", run=self.fake("b" * 40, []))

    def test_a_folder_inside_the_repository_or_not_empty_is_refused(self):
        with self.assertRaises(ValueError):
            lp.fetch(GOOD, ROOT / "build" / "lvgl", run=self.fake(GOOD["commit"], []))
        with self.assertRaises(ValueError):
            lp.fetch(GOOD, ROOT, run=self.fake(GOOD["commit"], []))
        with tempfile.TemporaryDirectory() as folder:
            (Path(folder) / "x").write_text("x", encoding="utf-8")
            with self.assertRaises(ValueError):
                lp.fetch(GOOD, Path(folder), run=self.fake(GOOD["commit"], []))

    def test_the_pin_is_checked_before_anything_runs(self):
        calls = []
        with tempfile.TemporaryDirectory() as folder, self.assertRaises(ValueError):
            lp.fetch(dict(GOOD, commit="main"), Path(folder) / "lvgl", run=self.fake("", calls))
        self.assertEqual(calls, [])


class Compile(unittest.TestCase):
    SIZE = "   text\t   data\t    bss\t    dec\t    hex\tfilename\n   1000\t     20\t     30\t   1050\t    41a\ta.o\n   2000\t     10\t      0\t   2010\t    7da\tb.o\n"

    def library(self, folder, names=("a", "b")):
        src = Path(folder) / "lib" / "src" / "core"
        src.mkdir(parents=True)
        for name in names:
            (src / f"{name}.c").write_text("int x;", encoding="utf-8")
        return Path(folder) / "lib"

    def runner(self, size_text, calls):
        def run(args, check=True, capture_output=False, text=False):
            calls.append(args)
            return SimpleNamespace(stdout=size_text)

        return run

    def test_the_sizes_are_summed(self):
        total = lp.parse_size(self.SIZE)
        self.assertEqual(total, {"text": 3000, "data": 30, "bss": 30})
        self.assertEqual(lp.flash_bytes(total), 3030)

    def test_it_compiles_every_source_for_the_cortex_m7_with_the_settings_of_the_repository(self):
        calls = []
        with tempfile.TemporaryDirectory() as folder:
            sizes = lp.compile_library(GOOD, self.library(folder), Path(folder) / "out", run=self.runner(self.SIZE, calls))
        compiles = [c for c in calls if c[0] == "arm-none-eabi-gcc"]
        self.assertEqual(len(compiles), 2)
        self.assertIn("-mcpu=cortex-m7", compiles[0])
        self.assertIn("-DLV_CONF_INCLUDE_SIMPLE", compiles[0])
        self.assertIn(f"-I{ROOT / 'config'}", compiles[0])
        self.assertEqual((sizes["objects"], sizes["flash"], sizes["bss"]), (2, 3030, 30))

    def test_a_library_over_the_budget_fails(self):
        with tempfile.TemporaryDirectory() as folder, self.assertRaises(RuntimeError):
            lp.compile_library(dict(GOOD, flash_budget_bytes=16384), self.library(folder), Path(folder) / "out", run=self.runner(self.SIZE.replace("1000", "20000"), []))

    def test_a_folder_without_sources_fails(self):
        with tempfile.TemporaryDirectory() as folder:
            (Path(folder) / "lib" / "src").mkdir(parents=True)
            with self.assertRaises(RuntimeError):
                lp.compile_library(GOOD, Path(folder) / "lib", Path(folder) / "out", run=self.runner(self.SIZE, []))

    def test_the_command_line(self):
        self.assertEqual(lp.main(["--check"]), 0)
        self.assertEqual(lp.main([]), 2)

    def test_the_command_line_stops_on_a_vendored_file(self):
        original = lp.vendored_files
        lp.vendored_files = lambda root=lp.ROOT: ["x/lvgl.h"]
        try:
            self.assertEqual(lp.main(["--check"]), 1)
        finally:
            lp.vendored_files = original


class WorkflowText(unittest.TestCase):
    def test_it_only_reads_and_fetches_outside_the_checkout(self):
        self.assertIn("permissions:\n  contents: read", WORKFLOW)
        self.assertIn('--fetch "$RUNNER_TEMP/lvgl"', WORKFLOW)
        self.assertIn('--compile "$RUNNER_TEMP/lvgl"', WORKFLOW)
        self.assertNotIn("secrets.", WORKFLOW)
        self.assertNotIn("inputs.", WORKFLOW)
        self.assertIn("persist-credentials: false", WORKFLOW)
        self.assertIn("timeout-minutes:", WORKFLOW)

    def test_it_starts_on_the_files_of_the_pin(self):
        for path in ("config/lvgl.cfg", "config/lv_conf.h", "tools/lvgl_pin.py"):
            self.assertEqual(WORKFLOW.count(f'"{path}"'), 2, path)

    def test_the_check_is_a_real_command(self):
        done = subprocess.run([sys.executable, "-I", str(ROOT / "tools" / "lvgl_pin.py"), "--check"], capture_output=True, text=True, check=False)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertIn("LVGL_PIN=PASS", done.stdout)


if __name__ == "__main__":
    unittest.main()
