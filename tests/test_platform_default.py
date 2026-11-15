# =============================================================================
# HYDRA-UMC - tests/test_platform_default.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The H757 board as the default platform, behind a switch that ships off (HYDRA_DEFAULT_PLATFORM_H757, platform_profile.h): the host test of the choice is in both runners with the five
cases that matter (no platform named, the switch off, the switch on, the platform named, the other platform named with the switch on), the switch is read only by the generated header, and
every source that decides on the platform gets the header first, so the switch cannot reach half of a build. Evidence: simulator (preprocessor, compiled in Actions)."""
from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ROOT / "src" / "mcu_stm32h757"
INCLUDE = re.compile(r'#\s*include\s+"([^"]+)"')
CONDITION = re.compile(r"^\s*#\s*(?:if|elif)\b.*\bHYDRA_PLATFORM_H757\b", re.MULTILINE)


def includes_the_header(path: Path, seen: set[Path] | None = None) -> bool:
    """True when the file gets platform_profile.h, directly or through another header of the sources, before it decides on a platform."""
    seen = seen or set()
    if path in seen:
        return False
    seen.add(path)
    text = path.read_text(encoding="utf-8", errors="replace")
    for name in INCLUDE.findall(text):
        if name == "platform_profile.h":
            return True
        for folder in (path.parent, SOURCES / "Common", SOURCES / "CM7", SOURCES / "CM4", SOURCES / "CM4" / "boot", SOURCES / "CM7" / "boot"):
            candidate = folder / name
            if candidate.is_file() and includes_the_header(candidate, seen):
                return True
    return False


class DefaultPlatformTests(unittest.TestCase):
    def test_the_host_test_is_in_both_runners_with_its_five_cases(self) -> None:
        runner = (ROOT / "tools" / "host_tests.py").read_text(encoding="utf-8")
        build = (ROOT / "tools" / "build_firmware.py").read_text(encoding="utf-8")
        self.assertEqual(runner.count("tests/test_platform_default.c"), 5)
        self.assertIn("test_platform_default", build)
        for expected in ('"EXPECT_PLATFORM=745"', '"EXPECT_PLATFORM=757"', '"HYDRA_DEFAULT_PLATFORM_H757=0"', '"HYDRA_DEFAULT_PLATFORM_H757=1"'):
            self.assertIn(expected, runner)
            self.assertIn(expected.replace('"', '"-D', 1), build)

    def test_the_switch_is_read_by_the_generated_header_and_nothing_else_in_the_sources(self) -> None:
        readers = [p.relative_to(ROOT).as_posix() for p in sorted((ROOT / "src").rglob("*")) if p.suffix in (".c", ".h") and "HYDRA_DEFAULT_PLATFORM_H757" in p.read_text(encoding="utf-8", errors="replace")]
        self.assertEqual(readers, ["src/mcu_stm32h757/Common/platform_profile.h"])

    def test_every_source_that_decides_on_the_platform_gets_the_header_first(self) -> None:
        deciding = [p for p in sorted(SOURCES.rglob("*")) if p.suffix in (".c", ".h") and p.name != "platform_profile.h" and CONDITION.search(p.read_text(encoding="utf-8", errors="replace"))]
        self.assertGreater(len(deciding), 5)
        missing = [p.relative_to(ROOT).as_posix() for p in deciding if not includes_the_header(p)]
        self.assertEqual(missing, [], "these sources decide on the platform without the header: the switch would not reach them")

    def test_the_helper_follows_an_include_and_stops_on_a_loop(self) -> None:
        self.assertTrue(includes_the_header(SOURCES / "Common" / "clock_config.h"))  # through clock_tree.h
        self.assertFalse(includes_the_header(ROOT / "tests" / "test_platform_default.py"))


if __name__ == "__main__":
    unittest.main()
