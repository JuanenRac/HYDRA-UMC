"""bump_version.py bumps the header it is given and nothing else."""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "bump_version.py"


def bump(header_text, prefix="FIRMWARE_VERSION"):
    with tempfile.TemporaryDirectory() as tmp:
        header = Path(tmp) / "bootloader_common.h"
        header.write_text(header_text, encoding="utf-8")
        result = subprocess.run(
            [sys.executable, str(SCRIPT), str(header), prefix],
            capture_output=True, text=True, check=False,
        )
        return result, header.read_text(encoding="utf-8")


HEADER = """#define BOOTLOADER_VERSION_MAJOR 0
#define BOOTLOADER_VERSION_MINOR 1
#define BOOTLOADER_VERSION_PATCH 7
#define FIRMWARE_VERSION_MAJOR {major}
#define FIRMWARE_VERSION_MINOR {minor}
#define FIRMWARE_VERSION_PATCH {patch}
{build}"""


class BumpVersionTest(unittest.TestCase):
    def header(self, major, minor, patch, build=None):
        extra = "" if build is None else f"#define FIRMWARE_VERSION_BUILD {build}\n"
        return HEADER.format(major=major, minor=minor, patch=patch, build=extra)

    def test_three_part_patch_and_carry(self):
        result, text = bump(self.header(0, 1, 8))
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout.strip(), "0.1.9")
        result, text = bump(self.header(0, 1, 9))
        self.assertEqual(result.stdout.strip(), "0.2.0")
        result, text = bump(self.header(0, 9, 9))
        self.assertEqual(result.stdout.strip(), "1.0.0")

    def test_only_the_requested_family_changes(self):
        _, text = bump(self.header(0, 1, 8))
        self.assertIn("#define BOOTLOADER_VERSION_PATCH 7", text)
        self.assertIn("#define FIRMWARE_VERSION_PATCH 9", text)

    def test_four_part_header_is_bumped_from_its_own_macros(self):
        result, text = bump(self.header(0, 8, 0, 9))
        self.assertEqual(result.stdout.strip(), "0.8.1.0")
        self.assertIn("#define FIRMWARE_VERSION_BUILD 0", text)

    def test_a_dormant_build_macro_does_not_make_an_old_version_four_part(self):
        # The G474 header carries FIRMWARE_VERSION_BUILD already, at 0, while the
        # version is still three-part: the patch must count, not the build.
        result, text = bump(self.header(0, 1, 8, 0))
        self.assertEqual(result.stdout.strip(), "0.1.9")
        self.assertIn("#define FIRMWARE_VERSION_BUILD 0", text)
        self.assertIn("#define FIRMWARE_VERSION_PATCH 9", text)

    def test_the_build_macro_takes_over_at_the_fourth_part_boundary(self):
        result, text = bump(self.header(0, 7, 9, 0))
        self.assertEqual(result.stdout.strip(), "0.8.0.0")
        self.assertIn("#define FIRMWARE_VERSION_MINOR 8", text)

    def test_never_asks_the_manifest(self):
        # The project manifest may already be four-part while a component
        # header is still three-part: the header alone decides.
        result, _ = bump(self.header(0, 1, 2))
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout.strip(), "0.1.3")


MANIFEST = {
    "schema_version": "1.0",
    "ecosystem": "HYDRA-UMC",
    "name": "HYDRA-UMC",
    "version": "0.2.4",
    "native_version": {
        "file": "src/boot/bootloader_common.h",
        "pattern": {
            "major": r"#define\s+FIRMWARE_VERSION_MAJOR\s+(\d+)",
            "minor": r"#define\s+FIRMWARE_VERSION_MINOR\s+(\d+)",
            "patch": r"#define\s+FIRMWARE_VERSION_PATCH\s+(\d+)",
            "build": r"#define\s+FIRMWARE_VERSION_BUILD\s+(\d+)",
        },
    },
}


def make_repository(root, manifest=None):
    """A minimal copy of the repository: the bump scripts, the project tool, a manifest, a changelog and a header."""
    import json
    import shutil

    for name in ("bump_version.py", "bump_manifest_version.py"):
        shutil.copy(ROOT / name, root / name)
    (root / "tools").mkdir()
    shutil.copy(ROOT / "tools" / "hydra_project_tool.py", root / "tools" / "hydra_project_tool.py")
    (root / "src" / "boot").mkdir(parents=True)
    (root / "src" / "boot" / "bootloader_common.h").write_text(
        BumpVersionTest().header(0, 2, 4, 0), encoding="utf-8"
    )
    (root / "hydra-umc.project.json").write_text(json.dumps(MANIFEST if manifest is None else manifest, indent=2), encoding="utf-8")
    (root / "CHANGELOG.md").write_text("# Changelog\n\n## [Unreleased]\n\n## [0.2.4] - Earlier\n\n- Something.\n", encoding="utf-8")


class BumpWithoutArgumentsTest(unittest.TestCase):
    """The repository bumps through `tools/hydra_project_tool.py bump` like every other one."""

    def test_without_arguments_it_bumps_the_header_the_manifest_names(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            make_repository(root)
            result = subprocess.run([sys.executable, str(root / "bump_version.py")], capture_output=True, text=True, check=False, cwd=root)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.strip(), "0.2.5")
            header = (root / "src" / "boot" / "bootloader_common.h").read_text(encoding="utf-8")
            self.assertIn("#define FIRMWARE_VERSION_PATCH 5", header)
            self.assertIn("#define BOOTLOADER_VERSION_PATCH 7", header)  # the other family is untouched

    def test_the_project_tool_bump_moves_the_header_and_the_manifest_together(self):
        import json

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            make_repository(root)
            for command in (["init", "-q"], ["add", "-A"], ["-c", "user.name=t", "-c", "user.email=t@example.invalid", "commit", "-q", "-m", "init"]):
                subprocess.run(["git", *command], cwd=root, check=True, capture_output=True)
            result = subprocess.run(
                [sys.executable, str(root / "tools" / "hydra_project_tool.py"), "bump"], capture_output=True, text=True, check=False, cwd=root
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PROJECT_TOOL=BUMPED 0.2.4 -> 0.2.5", result.stdout)
            self.assertEqual(json.loads((root / "hydra-umc.project.json").read_text(encoding="utf-8"))["version"], "0.2.5")
            self.assertIn("#define FIRMWARE_VERSION_PATCH 5", (root / "src" / "boot" / "bootloader_common.h").read_text(encoding="utf-8"))

    def test_without_a_firmware_header_in_the_manifest_it_stops_instead_of_guessing(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            make_repository(root, manifest={**MANIFEST, "native_version": {"file": "pyproject.toml", "pattern": "version = \"(\\d+)\\.(\\d+)\\.(\\d+)\""}})
            result = subprocess.run([sys.executable, str(root / "bump_version.py")], capture_output=True, text=True, check=False, cwd=root)
            self.assertEqual(result.returncode, 1)
            self.assertIn("does not name a FIRMWARE_VERSION header", result.stderr)

    def test_one_argument_is_still_a_usage_error(self):
        result = subprocess.run([sys.executable, str(SCRIPT), "only-one"], capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 1)
        self.assertIn("usage", result.stderr)


if __name__ == "__main__":
    unittest.main()
