# =============================================================================
# HYDRA-UMC - tests/test_release_workflow.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The release workflow (.github/workflows/release.yml): it only verifies and publishes what was signed on the maintainer's machine.

These are text checks on the file (no YAML library is needed), written so that a later edit cannot quietly widen who can start the job, give it a
signing key or publish something that was not verified. Also the section of the CHANGELOG that becomes the notes of the release.
"""
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import changelog_section  # noqa: E402

WORKFLOW = (ROOT / ".github" / "workflows" / "release.yml").read_text(encoding="utf-8").replace("\r\n", "\n")
CODE = "\n".join(line for line in WORKFLOW.split("\n") if not line.lstrip().startswith("#"))


def triggers() -> list[str]:
    block = re.search(r"(?m)^on:\n((?:  .*\n|\n)+)", CODE)
    assert block, "no on: block"
    return re.findall(r"(?m)^  ([a-z_]+):", block.group(1))


class WhoCanStartIt(unittest.TestCase):
    def test_it_is_started_by_hand_and_by_nothing_else(self):
        self.assertEqual(triggers(), ["workflow_dispatch"])
        for forbidden in ("pull_request_target", "pull_request:", "push:", "schedule:", "workflow_run", "issue_comment"):
            self.assertNotIn(forbidden, CODE)

    def test_it_asks_for_the_tag_it_will_publish(self):
        self.assertRegex(CODE, r"(?m)^      tag:\n")
        self.assertIn('"${REQUESTED_TAG}" != "v${version}"', CODE)

    def test_the_job_runs_from_main_only_and_inside_the_protected_environment(self):
        self.assertIn("if: ${{ github.ref == 'refs/heads/main' }}", CODE)
        self.assertRegex(CODE, r"(?m)^    environment: release$")

    def test_a_published_version_is_never_replaced_but_the_draft_is_not_mistaken_for_one(self):
        self.assertIn('gh release view "${REQUESTED_TAG}" --json isDraft', CODE)
        self.assertIn('"${state}" != "true"', CODE)


class WhatItMayDo(unittest.TestCase):
    def test_read_only_by_default_and_write_only_to_contents_in_the_job(self):
        self.assertRegex(CODE, r"(?m)^permissions:\n  contents: read\n")
        self.assertRegex(CODE, r"(?m)^    permissions:\n      contents: write\n")
        self.assertEqual(len(re.findall(r"(?m)^\s+[a-z-]+: write$", CODE)), 1)

    def test_every_action_is_pinned_by_commit(self):
        uses = re.findall(r"(?m)^\s+uses: (\S+)", CODE)
        self.assertTrue(uses)
        for item in uses:
            self.assertRegex(item, r"^[\w./-]+@[0-9a-f]{40}$", item)

    def test_the_checkout_does_not_keep_the_token_in_the_repository(self):
        self.assertIn("persist-credentials: false", CODE)


class NoKeysAnywhere(unittest.TestCase):
    def test_it_uses_no_secret_at_all(self):
        self.assertEqual(re.findall(r"\$\{\{\s*secrets\.", CODE), [])

    def test_nothing_signs_builds_or_reads_a_private_key(self):
        for forbidden in ("HYDRA_FW_SIGNING_SEED_FILE", "HYDRA_IMAGE_KEY", "HYDRA_UMC_RELEASE", "hydra-sign", "sign_release.py", "build_firmware",
                          "gcc-arm-none-eabi", "cryptography", ".pem", ".seed", "printenv", "set -x"):
            self.assertNotIn(forbidden, CODE)


class WhatItChecksBeforePublishing(unittest.TestCase):
    def test_the_files_are_verified_before_the_release_is_created(self):
        self.assertLess(CODE.index("tools/verify_release_set.py"), CODE.index("gh release create"))
        self.assertIn("tools/image_trust.py validate", CODE)

    def test_the_set_is_stopped_by_the_first_failure(self):
        self.assertIn("set -euo pipefail", CODE)
        self.assertNotIn("|| true", CODE)
        self.assertNotIn("continue-on-error", CODE)

    def test_the_release_is_made_from_the_verified_copy_not_from_the_draft(self):
        self.assertIn('cd "${RUNNER_TEMP}/signed"', CODE)
        self.assertIn('mapfile -t assets < "${RUNNER_TEMP}/assets.txt"', CODE)
        self.assertNotIn("--draft=false", CODE)

    def test_exactly_one_draft_of_the_tag_is_the_source(self):
        self.assertIn(".draft and .tag_name", CODE)
        self.assertIn('!= "1"', CODE)
        self.assertIn("grep -c .", CODE)

    def test_the_published_files_are_checked_again_after_the_upload_and_the_draft_is_removed_last(self):
        self.assertIn("gh release download", CODE)
        self.assertEqual(CODE.count("tools/verify_release_set.py"), 2)
        self.assertGreater(CODE.index("--method DELETE"), CODE.index("gh release download"))

    def test_the_notes_come_from_the_changelog_and_a_release_without_notes_stops(self):
        self.assertIn('tools/changelog_section.py "${version}"', CODE)


class ChangelogSectionTests(unittest.TestCase):
    TEXT = "# Changelog\n\n## [Unreleased]\n\n## [1.2.3] - Title\n\n- one\n- two\n\n## [1.2.2] - Older\n\n- old\n\n## [1.2.1] - Empty\n\n"

    def test_the_section_of_a_version_is_its_body(self):
        self.assertEqual(changelog_section.section(self.TEXT, "1.2.3"), "- one\n- two\n")
        self.assertTrue(changelog_section.section(self.TEXT, "1.2.3", True).startswith("## [1.2.3] - Title"))

    def test_a_missing_or_empty_section_is_none(self):
        self.assertIsNone(changelog_section.section(self.TEXT, "9.9.9"))
        self.assertIsNone(changelog_section.section(self.TEXT, "1.2.1"))
        self.assertIsNone(changelog_section.section(self.TEXT, "Unreleased"))

    def test_the_real_changelog_has_a_section_for_the_version_of_the_project(self):
        import json
        version = json.loads((ROOT / "hydra-umc.project.json").read_text(encoding="utf-8"))["version"]
        self.assertIsNotNone(changelog_section.section((ROOT / "CHANGELOG.md").read_text(encoding="utf-8"), version))


if __name__ == "__main__":
    unittest.main()
