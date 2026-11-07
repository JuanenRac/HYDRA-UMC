# =============================================================================
# HYDRA-UMC - tests/test_gen_node_vectors.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The generator of the C vector headers (tools/gen_node_vectors.py): the committed headers are what the JSON files give, a changed JSON is noticed, and the
rows of the slot scan windows say what the Python reference says. Evidence: source check."""
import contextlib
import io
import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import gen_node_vectors as gen  # noqa: E402
import slot_scan  # noqa: E402


def run(argv):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        code = gen.main(argv)
    return code, out.getvalue()


class GeneratedHeaders(unittest.TestCase):
    def test_the_committed_headers_are_what_the_json_files_give(self):
        code, output = run(["--check"])
        self.assertEqual(code, 0, output)
        self.assertIn("NODE_VECTORS=PASS", output)

    def test_a_header_that_is_out_of_date_is_noticed_and_writing_repairs_it(self):
        original = gen.TESTS
        with tempfile.TemporaryDirectory() as folder:
            copy = Path(folder)
            for name in ("estop_vectors.json", "slot_scan_vectors.json", "estop_vectors.h", "slot_scan_vectors.h"):
                shutil.copyfile(original / name, copy / name)
            gen.TESTS = copy
            try:
                self.assertEqual(run(["--check"])[0], 0)
                (copy / "slot_scan_vectors.h").write_text("/* stale */\n", encoding="utf-8")
                code, output = run(["--check"])
                self.assertEqual(code, 1)
                self.assertIn("NODE_VECTORS=FAIL tests/slot_scan_vectors.h", output)
                self.assertNotIn("estop_vectors.h is not", output)
                self.assertEqual(run([])[0], 0)
                self.assertEqual(run(["--check"])[0], 0)
                (copy / "estop_vectors.h").unlink()
                self.assertEqual(run(["--check"])[0], 1, "a missing header is a failure")
            finally:
                gen.TESTS = original

    def test_a_changed_json_changes_the_header(self):
        original = gen.TESTS
        with tempfile.TemporaryDirectory() as folder:
            copy = Path(folder)
            for name in ("estop_vectors.json", "slot_scan_vectors.json", "estop_vectors.h", "slot_scan_vectors.h"):
                shutil.copyfile(original / name, copy / name)
            data = json.loads((copy / "slot_scan_vectors.json").read_text(encoding="utf-8"))
            data["delays"].append({"uid32": 7, "delay_ms": 9})
            (copy / "slot_scan_vectors.json").write_text(json.dumps(data), encoding="utf-8")
            gen.TESTS = copy
            try:
                self.assertEqual(run(["--check"])[0], 1)
            finally:
                gen.TESTS = original


class WindowRows(unittest.TestCase):
    def setUp(self):
        self.data = json.loads((gen.TESTS / "slot_scan_vectors.json").read_text(encoding="utf-8"))
        self.header = gen.slot_scan_header()

    def test_there_is_one_row_per_scenario_and_slot(self):
        rows = [line for line in self.header.split("\n") if line.startswith('    {"') and "SlotScanVectorWindow" not in line and line.rstrip().endswith("},") and line.count("{") >= 4]
        self.assertEqual(len(rows), len(self.data["scenarios"]) * slot_scan.SLOTS)

    def test_the_verdict_codes_are_those_of_the_spi_link(self):
        import spi_system  # noqa: PLC0415

        self.assertEqual({"ABSENT": 0, "OK": 1, "DUPLICATE": 2, "MISPLACED": 3, "NOISY": 4}, spi_system.VERDICTS)
        used = {v for s in self.data["scenarios"] for v in s["expect"].values()}
        self.assertEqual(used, set(spi_system.VERDICTS), "the scenarios use every verdict")

    def test_every_expected_verdict_is_what_the_reference_gives(self):
        for scenario in self.data["scenarios"]:
            heard = {int(slot): slot_scan.Heard(tuple(bytes.fromhex(f) for f in h["frames"]), h["bus_errors"]) for slot, h in scenario["heard"].items()}
            result = slot_scan.scan(heard, scenario["scan_id"])
            self.assertEqual({str(k): v for k, v in result.items()}, scenario["expect"], scenario["name"])

    def test_a_scenario_with_more_frames_than_the_header_holds_is_refused(self):
        original = gen.TESTS
        with tempfile.TemporaryDirectory() as folder:
            copy = Path(folder)
            data = json.loads((original / "slot_scan_vectors.json").read_text(encoding="utf-8"))
            frame = data["replies"][0]["data_hex"]
            data["scenarios"].append({"name": "too many", "scan_id": 1, "heard": {"0": {"frames": [frame] * 5, "bus_errors": 0}}, "expect": {str(s): "ABSENT" for s in range(8)}})
            (copy / "slot_scan_vectors.json").write_text(json.dumps(data), encoding="utf-8")
            gen.TESTS = copy
            try:
                with self.assertRaises(SystemExit):
                    gen.slot_scan_header()
            finally:
                gen.TESTS = original

    def test_a_long_frame_is_refused_too(self):
        original = gen.TESTS
        with tempfile.TemporaryDirectory() as folder:
            copy = Path(folder)
            data = json.loads((original / "slot_scan_vectors.json").read_text(encoding="utf-8"))
            data["scenarios"].append({"name": "long", "scan_id": 1, "heard": {"0": {"frames": ["00" * 9], "bus_errors": 0}}, "expect": {str(s): "ABSENT" for s in range(8)}})
            (copy / "slot_scan_vectors.json").write_text(json.dumps(data), encoding="utf-8")
            gen.TESTS = copy
            try:
                with self.assertRaises(SystemExit):
                    gen.slot_scan_header()
            finally:
                gen.TESTS = original


if __name__ == "__main__":
    unittest.main()
