"""show_component_version.py only reads; bump_firmware_versions.py raises what it is told to; the build scripts do neither."""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

import bump_firmware_versions as bumper  # noqa: E402
import show_component_version as shower  # noqa: E402

HEADER = """#define BOOTLOADER_VERSION_MAJOR 0
#define BOOTLOADER_VERSION_MINOR 1
#define BOOTLOADER_VERSION_PATCH 7
#define FIRMWARE_VERSION_MAJOR 0
#define FIRMWARE_VERSION_MINOR 2
#define FIRMWARE_VERSION_PATCH 2
"""


class ShowComponentVersionTest(unittest.TestCase):
    def test_reads_each_family_and_changes_nothing(self):
        self.assertEqual(shower.read_version(HEADER, "BOOTLOADER_VERSION"), "0.1.7")
        self.assertEqual(shower.read_version(HEADER, "FIRMWARE_VERSION"), "0.2.2")

    def test_a_fourth_component_counts_only_from_the_four_part_version(self):
        old = HEADER + "#define FIRMWARE_VERSION_BUILD 3\n"
        self.assertEqual(shower.read_version(old, "FIRMWARE_VERSION"), "0.2.2")
        new = old.replace("FIRMWARE_VERSION_MINOR 2", "FIRMWARE_VERSION_MINOR 8").replace(
            "FIRMWARE_VERSION_MAJOR 0", "FIRMWARE_VERSION_MAJOR 0")
        self.assertEqual(shower.read_version(new, "FIRMWARE_VERSION"), "0.8.2.3")

    def test_a_missing_macro_is_an_error(self):
        with self.assertRaises(ValueError):
            shower.read_version("#define X 1\n", "FIRMWARE_VERSION")

    def test_command_line(self):
        with tempfile.TemporaryDirectory() as tmp:
            header = Path(tmp) / "bootloader_common.h"
            header.write_text(HEADER, encoding="utf-8")
            result = subprocess.run(
                [sys.executable, str(ROOT / "show_component_version.py"), str(header), "FIRMWARE_VERSION"],
                capture_output=True, text=True, check=False,
            )
            self.assertEqual((result.returncode, result.stdout.strip()), (0, "0.2.2"))
            self.assertEqual(header.read_text(encoding="utf-8"), HEADER)


class BumpFirmwareVersionsTest(unittest.TestCase):
    def test_every_component_points_at_a_real_header_with_its_macros(self):
        for name, (header, prefix, _) in bumper.COMPONENTS.items():
            text = (ROOT / header).read_text(encoding="utf-8", errors="replace")
            self.assertRegex(text, rf"#define\s+{prefix}_PATCH\s+\d+", name)

    def test_unknown_component_is_refused_before_anything_changes(self):
        self.assertEqual(bumper.main(["no-such-component"]), 2)

    def test_list_does_not_change_anything(self):
        before = {n: bumper.current(n) for n in bumper.COMPONENTS}
        self.assertEqual(bumper.main(["--list"]), 0)
        self.assertEqual(before, {n: bumper.current(n) for n in bumper.COMPONENTS})


class BuildScriptsDoNotBumpTest(unittest.TestCase):
    def test_no_build_script_runs_a_bump(self):
        for script in ("build_firmware.sh", "build_firmware.bat"):
            for number, line in enumerate((ROOT / script).read_text(encoding="utf-8").splitlines(), start=1):
                code = line.strip().lower()
                if not code or code.startswith(("#", "rem ", "echo", "printf")):
                    continue
                self.assertNotIn("bump_version", code, f"{script}:{number}")
                self.assertNotIn("bump_manifest_version", code, f"{script}:{number}")


if __name__ == "__main__":
    unittest.main()
