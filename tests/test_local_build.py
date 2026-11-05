"""The host C tests compile only on a CI runner or with ALLOW_LOCAL_BUILD=1 (tools/local_build.py). Nothing here compiles."""
from __future__ import annotations

import contextlib
import io
import os
import sys
import unittest
from pathlib import Path
from unittest import mock

TOOLS = Path(__file__).resolve().parent.parent / "tools"
sys.path.insert(0, str(TOOLS))

import host_tests  # noqa: E402
import local_build  # noqa: E402

NO_CI = {name: "" for name in ("CI", "GITHUB_ACTIONS", "ALLOW_LOCAL_BUILD")}


class LockTests(unittest.TestCase):
    def test_the_rule(self):
        self.assertFalse(local_build.local_build_allowed({}))
        self.assertTrue(local_build.local_build_allowed({"CI": "true"}))
        self.assertTrue(local_build.local_build_allowed({"GITHUB_ACTIONS": "true"}))
        self.assertTrue(local_build.local_build_allowed({"ALLOW_LOCAL_BUILD": "1"}))
        self.assertFalse(local_build.local_build_allowed({"CI": "false", "ALLOW_LOCAL_BUILD": "0"}))

    def test_no_compiler_is_found_on_a_plain_pc_and_nothing_is_run(self):
        out = io.StringIO()
        with mock.patch.dict(os.environ, NO_CI), mock.patch.object(host_tests.subprocess, "run", side_effect=AssertionError("compiled")), \
                contextlib.redirect_stdout(out):
            self.assertIsNone(host_tests.find_compiler())
            self.assertEqual(host_tests.main([]), 0)
        self.assertIn("HOST_TESTS=SKIPPED", out.getvalue())
        self.assertIn("ALLOW_LOCAL_BUILD=1", out.getvalue())

    def test_a_ci_runner_looks_for_the_compiler_as_before(self):
        with mock.patch.dict(os.environ, {**NO_CI, "CI": "true", "HOST_CC": ""}), mock.patch.object(host_tests.shutil, "which", return_value="/usr/bin/cc"):
            self.assertEqual(host_tests.find_compiler(), "/usr/bin/cc")


if __name__ == "__main__":
    unittest.main()
