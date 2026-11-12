# =============================================================================
# HYDRA-UMC - tests/test_h745_invariance.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The comparison that shows a change leaves the images of the default platform untouched (tools/h745_invariance.py) and the manual workflow that runs it on two commits. Evidence:
simulator (the comparison runs on sums and on files made by the test); the real proof is the workflow run on two commits."""
import io
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import h745_invariance as inv  # noqa: E402

WORKFLOW = (ROOT / ".github/workflows/h745-invariance.yml").read_text(encoding="utf-8").replace("\r\n", "\n")


def make_images(directory: Path, version: str = "0.1.8", change: str | None = None) -> None:
    for component in inv.COMPONENTS:
        data = component.encode() * 40 + (b"changed" if component == change else b"")
        (directory / f"HYDRA_{component}_v{version}.bin").write_bytes(data)


class InvarianceTests(unittest.TestCase):
    def test_a_directory_of_images_gives_one_sum_per_component_without_the_version(self):
        with tempfile.TemporaryDirectory() as tmp:
            make_images(Path(tmp), "0.1.8")
            sums = inv.collect(Path(tmp))
            self.assertEqual(sorted(sums), sorted(inv.COMPONENTS))
            self.assertEqual(inv.parse(inv.render(sums)), sums)

    def test_the_same_code_under_another_version_in_the_file_name_gives_the_same_sums(self):
        with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
            make_images(Path(a), "0.1.8")
            make_images(Path(b), "0.2.0")
            self.assertEqual(inv.compare(inv.collect(Path(a)), inv.collect(Path(b))), [])

    def test_one_changed_byte_in_one_image_is_named(self):
        with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
            make_images(Path(a))
            make_images(Path(b), change="KB_CM4_BOOTLOADER")
            found = inv.compare(inv.collect(Path(a)), inv.collect(Path(b)))
            self.assertEqual(len(found), 1)
            self.assertIn("KB_CM4_BOOTLOADER", found[0])

    def test_a_missing_image_is_refused_when_collecting(self):
        with tempfile.TemporaryDirectory() as tmp:
            make_images(Path(tmp))
            (Path(tmp) / "HYDRA_RCB_APP_v0.1.8.bin").unlink()
            with self.assertRaises(ValueError):
                inv.collect(Path(tmp))

    def test_a_component_only_one_side_has_is_a_difference(self):
        base = {"KB_CM4_APP": "a" * 64}
        head = {"KB_CM4_APP": "a" * 64, "KB_CM7_APP": "b" * 64}
        self.assertEqual(len(inv.compare(base, head)), 1)
        self.assertEqual(len(inv.compare(head, base)), 1)

    def test_a_line_that_is_not_a_sum_is_refused(self):
        with self.assertRaises(ValueError):
            inv.parse("not a sum\n")
        with self.assertRaises(ValueError):
            inv.parse("xyz  KB_CM4_APP\n")

    def test_the_command_line_exits_one_on_a_difference_and_zero_on_none(self):
        with tempfile.TemporaryDirectory() as tmp:
            same = Path(tmp) / "same.txt"
            other = Path(tmp) / "other.txt"
            same.write_text(inv.render({c: "c" * 64 for c in inv.COMPONENTS}), encoding="utf-8")
            other.write_text(inv.render({c: ("d" if c == "RCB_APP" else "c") * 64 for c in inv.COMPONENTS}), encoding="utf-8")
            out, err = io.StringIO(), io.StringIO()
            with redirect_stdout(out), redirect_stderr(err):
                self.assertEqual(inv.main(["compare", str(same), str(same)]), 0)
                self.assertEqual(inv.main(["compare", str(same), str(other)]), 1)
            self.assertIn("H745_INVARIANCE=PASS", out.getvalue())
            self.assertIn("H745_INVARIANCE=FAIL", out.getvalue())
            self.assertIn("RCB_APP", err.getvalue())


class WorkflowTests(unittest.TestCase):
    def test_it_is_started_by_hand_and_only_by_the_owner(self):
        self.assertIn("workflow_dispatch:", WORKFLOW)
        self.assertNotRegex(WORKFLOW, r"(?m)^  (push|pull_request|schedule):")
        self.assertIn('[ "$ACTOR" = "$OWNER" ]', WORKFLOW)

    def test_both_commits_are_pinned_and_built_in_the_same_layout(self):
        self.assertIn("base_sha=", WORKFLOW)
        self.assertIn("head_sha=", WORKFLOW)
        self.assertIn("side: [base, head]", WORKFLOW)
        self.assertIn("ref: ${{ matrix.side == 'base' && needs.plan.outputs.base_sha || needs.plan.outputs.head_sha }}", WORKFLOW)

    def test_the_refs_are_checked_for_odd_characters_before_use(self):
        self.assertIn("A-Za-z0-9._/-", WORKFLOW)
        self.assertLess(WORKFLOW.index("Check the inputs"), WORKFLOW.index("Pin the two commits"))

    def test_it_holds_no_secret_makes_no_release_and_keeps_only_the_sums(self):
        self.assertNotIn("secrets.", WORKFLOW)
        self.assertNotIn("attest", WORKFLOW)
        self.assertNotIn("gh release", WORKFLOW)
        self.assertEqual(WORKFLOW.count("upload-artifact@"), 1)
        self.assertIn("path: sums-${{ matrix.side }}.txt", WORKFLOW)

    def test_the_build_runs_in_ci_mode_without_a_version_bump(self):
        self.assertIn('HYDRA_UMC_CI: "1"', WORKFLOW)
        self.assertIn('HYDRA_UMC_RELEASE: "0"', WORKFLOW)

    def test_the_comparison_fails_the_job_on_a_difference(self):
        compare = WORKFLOW.split("name: Compare\n", 1)[1]
        self.assertIn("tools/h745_invariance.py compare", compare)
        self.assertIn("| tee", compare)   # with pipefail on, the exit code of the comparison is the one of the step


if __name__ == "__main__":
    unittest.main()
