# =============================================================================
# HYDRA-UMC - tests/test_firmware_base_commands.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The four commands of tools/sync_firmware_base.py (status, check, sync, import) on a small set of fake chip directories, and the size gate of
tools/bootloader_size.py with a stand-in size tool. Nothing here touches the real bootloaders. Evidence: source check."""
import contextlib
import io
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import bootloader_size as size  # noqa: E402
import sync_firmware_base as base  # noqa: E402


class FakeChips(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        root = Path(self.temp.name)
        self.base_dir = root / "base"
        self.list_file = root / "shared.json"
        self.chips = {name: root / name for name in ("g474", "h745_cm4", "h745_cm7")}
        for directory in self.chips.values():
            directory.mkdir()
        patches = [mock.patch.object(base, "BASE", self.base_dir), mock.patch.object(base, "LIST", self.list_file), mock.patch.object(base, "CHIPS", self.chips)]
        for patch in patches:
            patch.start()
            self.addCleanup(patch.stop)
        self.addCleanup(self.temp.cleanup)

    def put(self, name, text, chips=None):
        for chip in chips or self.chips:
            (self.chips[chip] / name).write_bytes(text.encode())

    def run_command(self, command):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = base.main([command])
        return code, out.getvalue()

    def test_import_takes_only_what_is_identical_in_all_three_and_then_check_passes(self):
        self.put("same.h", "int a;\n")
        self.put("differs.h", "int b;\n", ["g474"])
        self.put("differs.h", "int c;\n", ["h745_cm4", "h745_cm7"])
        self.put("only_one.h", "int d;\n", ["g474"])
        code, output = self.run_command("import")
        self.assertEqual(code, 0, output)
        self.assertEqual(json.loads(self.list_file.read_text())["shared"], ["same.h"])
        self.assertEqual((self.base_dir / "same.h").read_text(), "int a;\n")
        self.assertIn("FIRMWARE_BASE=PASS", output)
        self.assertFalse((self.base_dir / "differs.h").exists())

    def test_a_drifted_copy_is_told_and_sync_repairs_it(self):
        self.put("same.h", "int a;\n")
        self.assertEqual(self.run_command("import")[0], 0)
        (self.chips["h745_cm7"] / "same.h").write_text("int drifted;\n")
        code, output = self.run_command("check")
        self.assertEqual(code, 1)
        self.assertIn("FIRMWARE_BASE=FAIL (1)", output)
        self.assertIn("h745_cm7", output)
        code, output = self.run_command("sync")
        self.assertEqual(code, 0, output)
        self.assertEqual((self.chips["h745_cm7"] / "same.h").read_text(), "int a;\n")
        self.assertEqual(output.count("wrote "), 1, "only the drifted copy is written")

    def test_a_missing_copy_and_a_file_that_is_not_in_the_list_are_told(self):
        self.put("same.h", "int a;\n")
        self.run_command("import")
        (self.chips["g474"] / "same.h").unlink()
        self.assertTrue(any("missing" in line for line in base.problems()))
        self.run_command("sync")
        self.assertEqual(base.problems(), [])
        self.put("extra.h", "int e;\n")
        self.base_dir.joinpath("extra.h").write_text("int e;\n")
        self.assertTrue(any("extra.h" in line and "not in shared.json" in line for line in base.problems()))

    def test_sync_with_an_empty_list_does_nothing(self):
        code, output = self.run_command("sync")
        self.assertEqual(code, 0)
        self.assertNotIn("wrote ", output)
        self.assertEqual(base.read_list(), [])

    def test_status_says_what_is_shared_what_differs_and_by_how_many_lines(self):
        self.put("same.h", "int a;\n")
        self.run_command("import")
        self.put("differs.h", "1\n2\n3\n", ["g474"])
        self.put("differs.h", "1\n2\n4\n5\n", ["h745_cm4"])
        self.put("differs.h", "1\n2\n3\n", ["h745_cm7"])
        self.put("alone.h", "x\n", ["h745_cm4"])
        self.put("equal_but_not_shared.h", "same\n")
        code, output = self.run_command("status")
        self.assertEqual(code, 0)
        self.assertIn("shared: 1 files", output)
        self.assertIn("same.h", output)
        self.assertRegex(output, r"differs\.h: differs \(lines against g474: h745_cm4 \d+, h745_cm7 0\)")
        self.assertIn("alone.h: only h745_cm4", output)
        self.assertIn("equal_but_not_shared.h: identical", output)

    def test_only_c_and_h_files_are_looked_at(self):
        self.put("notes.txt", "text\n")
        self.put("same.h", "int a;\n")
        self.assertEqual(base.identical_everywhere(), ["same.h"])
        self.assertNotIn("notes.txt", base.chip_sources(self.chips["g474"]))

    def test_the_difference_in_lines(self):
        self.assertEqual(base.line_difference(b"a\nb\n", b"a\nb\n"), 0)
        self.assertGreater(base.line_difference(b"a\nb\n", b"a\nc\n"), 0)
        self.assertEqual(base.line_difference(b"", b""), 0)


class SizeGate(unittest.TestCase):
    LINKER = "MEMORY\n{\n  FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 128K\n  RAM (xrw) : ORIGIN = 0x20000000, LENGTH = 32K\n}\n"

    def test_the_regions_of_a_linker_script(self):
        self.assertEqual(size.regions(self.LINKER), {"FLASH": 131072, "RAM": 32768})
        self.assertEqual(size.regions("RAM (rw) : ORIGIN = 0, LENGTH = 1M"), {"RAM": 1048576})
        self.assertEqual(size.regions("nothing"), {})

    def test_the_last_line_of_the_size_tool(self):
        self.assertEqual(size.parse_size("   text\t   data\t    bss\t    dec\t    hex\tfilename\n  1000\t  20\t  300\t  1320\t  528\tx.elf\n"), (1000, 20, 300))

    def make_entry(self, folder, flash_margin=100, ram_margin=100, glob="boot.elf"):
        (folder / "link.ld").write_text(self.LINKER)
        (folder / "boot.elf").write_bytes(b"elf")
        return {"elf_glob": glob, "linker": "link.ld", "flash_margin_bytes": flash_margin, "ram_margin_bytes": ram_margin}

    def evaluate(self, entry, folder, text=1000, data=20, bss=300):
        done = subprocess.CompletedProcess([], 0, stdout=f"text data bss dec hex filename\n{text} {data} {bss} 0 0 x.elf\n")
        with mock.patch.object(size, "ROOT", folder), mock.patch.object(size.subprocess, "run", return_value=done):
            return size.evaluate({"bootloaders": {"fake": entry}})

    def test_a_bootloader_with_room_passes(self):
        with tempfile.TemporaryDirectory() as folder:
            entry = self.make_entry(Path(folder))
            results, problems = self.evaluate(entry, Path(folder))
        self.assertEqual(problems, [])
        row = results[0][1]
        self.assertEqual((row["flash_used"], row["ram_used"], row["flash_free"], row["ram_free"]), (1020, 320, 131072 - 1020, 32768 - 320))

    def test_a_bootloader_that_leaves_less_than_the_margin_fails_on_flash_and_on_ram(self):
        with tempfile.TemporaryDirectory() as folder:
            entry = self.make_entry(Path(folder), flash_margin=131072, ram_margin=32768)
            _results, problems = self.evaluate(entry, Path(folder))
        self.assertEqual(len(problems), 2)
        self.assertIn("of flash free", problems[0])
        self.assertIn("of RAM free", problems[1])

    def test_exactly_the_margin_is_enough(self):
        with tempfile.TemporaryDirectory() as folder:
            entry = self.make_entry(Path(folder), flash_margin=131072 - 1020, ram_margin=32768 - 320)
            self.assertEqual(self.evaluate(entry, Path(folder))[1], [])

    def test_a_missing_or_ambiguous_elf_and_a_missing_region_are_problems_not_crashes(self):
        with tempfile.TemporaryDirectory() as folder:
            entry = self.make_entry(Path(folder), glob="none.elf")
            self.assertIn("expected one ELF", self.evaluate(entry, Path(folder))[1][0])
            (Path(folder) / "a.elf").write_bytes(b"1")
            (Path(folder) / "b.elf").write_bytes(b"2")
            self.assertIn("found 2", self.evaluate(dict(entry, elf_glob="[ab].elf"), Path(folder))[1][0])
            (Path(folder) / "link.ld").write_text("MEMORY { FLASH (rx) : ORIGIN = 0, LENGTH = 1K }")
            self.assertIn("not found", self.evaluate(dict(entry, elf_glob="boot.elf"), Path(folder))[1][0])

    def test_the_table_names_the_bootloaders_and_the_too_big_ones(self):
        row = {"text": 1, "data": 2, "bss": 3, "flash_used": 3, "flash_region": 10, "flash_free": 7, "flash_margin": 5, "ram_used": 5, "ram_region": 20, "ram_free": 15, "ram_margin": 5}
        text = size.markdown([("g474", row)], ["g474: too big"])
        self.assertIn("| g474 | 1 | 2 | 3 |", text)
        self.assertIn("**Too big:**", text)
        self.assertNotIn("Too big", size.markdown([("g474", row)], []))

    def test_the_command_line_fails_on_a_problem_and_appends_to_the_summary(self):
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            entry = self.make_entry(folder, flash_margin=10**9)
            config = folder / "cfg.json"
            config.write_text(json.dumps({"bootloaders": {"fake": entry}}))
            summary = folder / "summary.md"
            done = subprocess.CompletedProcess([], 0, stdout="t d b\n1000 20 300\n")
            out, err = io.StringIO(), io.StringIO()
            with mock.patch.object(size, "ROOT", folder), mock.patch.object(size.subprocess, "run", return_value=done), contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                code = size.main(["--config", str(config), "--summary", str(summary)])
            self.assertEqual(code, 1)
            self.assertIn("::error::fake", err.getvalue())
            self.assertIn("Bootloader size", summary.read_text())


if __name__ == "__main__":
    unittest.main()
