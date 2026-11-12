# =============================================================================
# HYDRA-UMC - tests/test_platform_header.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The platform header the firmware will read is what docs/platform_profiles.json says, for each platform. Evidence: simulator (the header is parsed by the test)."""
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import gen_platform_header as gen  # noqa: E402
import platform_profile as pp  # noqa: E402


def block(text: str, macro: str) -> dict[str, str]:
    """The #define lines between `#if/#elif defined(macro)` and the next `#elif` or `#endif` at that level."""
    lines = text.splitlines()
    start = next(i for i, line in enumerate(lines) if re.match(rf"#(if|elif) defined\({macro}\)$", line))
    values: dict[str, str] = {}
    depth = 0
    for line in lines[start + 1 :]:
        if line.startswith("#ifndef"):
            depth += 1
        elif line.startswith("#endif") and depth:
            depth -= 1
        elif line.startswith(("#elif", "#endif")):
            break
        match = re.match(r"#define (\w+) ([^ /]+)", line)
        if match:
            values[match.group(1)] = match.group(2)
    return values


def number(token: str) -> int:
    return int(re.sub(r"[UL]+$", "", token), 0)


class PlatformHeaderTests(unittest.TestCase):
    def setUp(self):
        self.document = pp.load()
        self.text = gen.render(self.document)

    def test_the_header_in_the_repository_is_what_the_profiles_say(self):
        self.assertEqual(gen.HEADER_PATH.read_bytes().decode("utf-8").replace("\r\n", "\n"), self.text)
        self.assertEqual(gen.main(["--check"]), 0)

    def test_each_platform_block_carries_the_numbers_of_its_profile(self):
        for macro, name in (("HYDRA_PLATFORM_H745", "h745_current"), ("HYDRA_PLATFORM_H757", "h757_target")):
            profile = self.document["profiles"][name]
            values = block(self.text, macro)
            bus = profile["stack_bus"]
            self.assertEqual(number(values["HYDRA_STACK_BUS_PRESCALER"]), bus["prescaler"], name)
            self.assertEqual(number(values["HYDRA_STACK_BUS_TIME_SEG1"]), bus["time_segment_1"], name)
            self.assertEqual(number(values["HYDRA_STACK_BUS_TIME_SEG2"]), bus["time_segment_2"], name)
            self.assertEqual(number(values["HYDRA_STACK_BUS_SYNC_JUMP_WIDTH"]), bus["sync_jump_width"], name)
            self.assertEqual(number(values["HYDRA_STACK_BUS_KERNEL_CLOCK_HZ"]), bus["kernel_clock_hz"], name)
            self.assertEqual(number(values["HYDRA_STACK_BUS_BIT_RATE_BPS"]), 1_000_000, name)
            for peripheral, owner in profile["peripheral_owner"].items():
                self.assertEqual(values[f"HYDRA_OWNER_{peripheral}"], f"HYDRA_OWNER_{owner}", f"{name} {peripheral}")
            for region in profile["memory_map"]["regions"]:
                base = f"HYDRA_MEM_{region['name']}"
                self.assertEqual(number(values[f"{base}_ORIGIN"]), region["origin"], f"{name} {region['name']}")
                self.assertEqual(number(values[f"{base}_LENGTH"]), region["length"], f"{name} {region['name']}")

    def test_the_default_platform_is_the_one_the_sources_build_for_and_its_role_is_slave(self):
        self.assertIn("#define HYDRA_PLATFORM_H745 1", self.text)
        self.assertEqual(block(self.text, "HYDRA_PLATFORM_H745")["HYDRA_SPI_ROLE"], "HYDRA_SPI_ROLE_SLAVE")

    def test_the_production_board_is_the_spi_master_by_default_and_the_role_can_be_set(self):
        self.assertEqual(block(self.text, "HYDRA_PLATFORM_H757")["HYDRA_SPI_ROLE"], "HYDRA_SPI_ROLE_MASTER")
        self.assertEqual(self.text.count("#ifndef HYDRA_SPI_ROLE"), 2)  # a build may set the role on the command line

    def test_the_cm7_owns_the_host_peripherals_of_the_production_board_and_the_cm4_those_of_today(self):
        h757 = block(self.text, "HYDRA_PLATFORM_H757")
        h745 = block(self.text, "HYDRA_PLATFORM_H745")
        for peripheral in ("SPI1", "FDCAN1", "I2C4"):
            self.assertEqual(h757[f"HYDRA_OWNER_{peripheral}"], "HYDRA_OWNER_CM7")
            self.assertEqual(h745[f"HYDRA_OWNER_{peripheral}"], "HYDRA_OWNER_CM4")

    def test_a_second_platform_or_a_wrong_role_does_not_compile(self):
        self.assertIn("#error \"choose one platform", self.text)
        self.assertIn("#error \"HYDRA_SPI_ROLE must be", self.text)
        self.assertTrue(self.text.endswith("#endif /* HYDRA_PLATFORM_PROFILE_H */\n"))

    def test_the_header_is_not_included_by_any_source_yet(self):
        for path in (ROOT / "src").rglob("*.[ch]"):
            if path.name != "platform_profile.h":
                self.assertNotIn("platform_profile.h", path.read_text(encoding="utf-8", errors="ignore"), str(path))


if __name__ == "__main__":
    unittest.main()
