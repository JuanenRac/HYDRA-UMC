# =============================================================================
# HYDRA-UMC - tests/test_workflow_firmware_build.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The manual firmware build may compile a branch that is not in main only as a dry run with the dev trust table, and stays closed for everything that can be released.

The rule is read from the workflow text (no YAML library is needed): who may start it, what is pinned, what a branch may and may not do. Evidence: simulator (text checks)."""
import re
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TEXT = (ROOT / ".github" / "workflows" / "firmware-build.yml").read_text(encoding="utf-8").replace("\r\n", "\n")


def step(name_part: str) -> str:
    """The text of the step whose name contains `name_part`, up to the next step."""
    start = TEXT.index(name_part)
    start = TEXT.rfind("\n      - name:", 0, start) + 1
    end = TEXT.find("\n      - name:", start + 10)
    job = re.compile(r"\n  [A-Za-z]").search(TEXT, start + 10)
    job_end = job.start() if job else -1
    candidates = [x for x in (end, job_end) if x != -1]
    return TEXT[start : min(candidates) if candidates else len(TEXT)]


class FirmwareBuildWorkflowTests(unittest.TestCase):
    def test_it_is_started_by_hand_and_only_by_the_owner(self):
        self.assertIn("workflow_dispatch:", TEXT)
        self.assertNotRegex(TEXT, r"(?m)^  (push|pull_request|schedule):")
        owner = step("Only the owner starts a build")
        self.assertIn('[ "$ACTOR" = "$OWNER" ]', owner)
        self.assertIn("only the owner of the repository starts a build", owner)

    def test_a_dry_run_is_the_default_and_a_draft_needs_it_off(self):
        dry = re.search(r"dry_run:\n(?:        .*\n)+", TEXT).group(0)
        self.assertIn("default: true", dry)
        self.assertRegex(TEXT, r"name: Prepare the draft pre-release \(owner approval\)\n(?:    .*\n){0,3}?    if: \$\{\{ !inputs\.dry_run \}\}")
        self.assertGreaterEqual(TEXT.count("if: ${{ !inputs.dry_run }}"), 2)  # the draft job and the step that creates the pre-release

    def test_the_main_check_refuses_everything_but_a_dry_run_with_the_dev_table(self):
        check = step("Make sure the commit belongs to the main branch")
        self.assertIn("git merge-base --is-ancestor HEAD refs/remotes/origin/main", check)
        self.assertIn('[ "$DRY_RUN" = "true" ] && [ "$FLAVOUR" = "dev" ]', check)
        self.assertIn("DRY_RUN: ${{ inputs.dry_run }}", check)
        self.assertIn("FLAVOUR: ${{ inputs.flavour }}", check)
        # the only way past the check for a commit that is not in main is the branch of the dry run; the other branch exits with an error
        self.assertRegex(check, r"else\n\s+echo \"::error::.*not part of the main branch.*exit 1")
        self.assertEqual(check.count("elif"), 1)

    def test_a_branch_build_is_announced_in_the_summary(self):
        self.assertIn("is NOT in main", step("Make sure the commit belongs to the main branch"))
        self.assertIn("GITHUB_STEP_SUMMARY", step("Make sure the commit belongs to the main branch"))

    def test_the_commit_is_pinned_in_the_plan_and_checked_out_again_by_its_sha(self):
        self.assertIn('echo "sha=$(git rev-parse HEAD)" >> "$GITHUB_OUTPUT"', step("Work out the version and pin the commit"))
        self.assertIn("ref: ${{ needs.plan.outputs.sha }}", TEXT)

    def test_the_ref_is_checked_for_odd_characters_before_it_is_used(self):
        self.assertIn("A-Za-z0-9._/-", step("Check the inputs"))
        self.assertLess(TEXT.index("Check the inputs"), TEXT.index("Check out this repository"))

    def test_it_holds_no_secret_and_signs_nothing(self):
        self.assertNotIn("secrets.", TEXT.replace("secrets.GITHUB_TOKEN", ""))
        self.assertIn("It holds no key and signs nothing", TEXT)
        self.assertNotRegex(TEXT, r"(?i)gpg|sign_release\.py\s+--key|private[_ ]key\s*[:=]")

    def test_the_rule_is_written_at_the_top_of_the_workflow(self):
        head = TEXT.split("name: Firmware build (manual)")[0]
        self.assertIn("A dry run with the dev table may build a branch", head)
        self.assertIn("its artifact is unsigned", head)

    def test_the_workflow_is_valid_for_the_syntax_checker_when_one_is_installed(self):
        try:
            result = subprocess.run(["actionlint", "-version"], capture_output=True, text=True, timeout=10)
        except (OSError, subprocess.SubprocessError):
            self.skipTest("actionlint is not installed")
        self.assertEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
