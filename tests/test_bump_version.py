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


if __name__ == "__main__":
    unittest.main()
