# =============================================================================
# HYDRA-UMC - tests/test_boot_roles.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The role of the bootloader of each core (boot_roles.h) follows the owner of the host link in the platform profile, the header defines nothing else, and the build runs its host test
four times and compiles the sources for the H757 platform without linking or shipping them. Evidence: simulator (text checks); the host test runs the four builds."""
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import platform_profile as pp  # noqa: E402

HEADER = (ROOT / "src/mcu_stm32h757/Common/boot_roles.h").read_text(encoding="utf-8")
SCRIPT = (ROOT / "build_firmware.sh").read_text(encoding="utf-8").replace("\r\n", "\n")


class BootRolesTests(unittest.TestCase):
    def test_the_profile_gives_one_owner_for_the_three_host_side_peripherals(self):
        for name in ("h745_current", "h757_target"):
            owner = pp.profile(name)["peripheral_owner"]
            self.assertEqual(len({owner["SPI1"], owner["FDCAN1"], owner["I2C4"]}), 1, name)

    def test_the_default_platform_keeps_the_cm4_as_gateway_and_the_h757_one_makes_the_cm7_the_gateway(self):
        self.assertEqual(pp.profile("h745_current")["peripheral_owner"]["SPI1"], "CM4")
        self.assertEqual(pp.profile("h757_target")["peripheral_owner"]["SPI1"], "CM7")

    def test_the_header_defines_only_the_role_macros(self):
        defined = set(re.findall(r"#define (\w+)", HEADER))
        self.assertEqual(defined - {"HYDRA_BOOT_ROLES_H"}, {"HYDRA_BOOT_THIS_CORE", "HYDRA_BOOT_OTHER_CORE", "HYDRA_BOOT_ROLE_GATEWAY", "HYDRA_BOOT_ROLE_BACKEND"})
        self.assertNotIn("HAL_", HEADER)
        self.assertNotRegex(HEADER, r"\bstatic\s+inline\b")   # no code: including it changes no object

    def test_the_host_test_is_run_for_both_cores_and_both_platforms_in_both_runners(self):
        host = (ROOT / "tools/host_tests.py").read_text(encoding="utf-8")
        self.assertEqual(host.count('"tests/test_boot_roles.c"'), 4)
        for core in ("CORE_CM4", "CORE_CM7"):
            self.assertIn(core, host)
        self.assertIn("tests/test_boot_roles.c", SCRIPT)
        self.assertIn('"-DCORE_CM4 -DHYDRA_PLATFORM_H757"', SCRIPT)
        self.assertIn('"-DCORE_CM7 -DHYDRA_PLATFORM_H757"', SCRIPT)

    def test_the_compile_check_for_the_h757_platform_is_after_the_images_and_never_links_or_ships(self):
        step = SCRIPT.split('step "10b.', 1)[1].split('step "11.', 1)[0]
        self.assertGreater(SCRIPT.index('step "10b.'), SCRIPT.index('step "10.'))
        self.assertIn("-DHYDRA_PLATFORM_H757", step)
        self.assertIn("compile_dir", step)
        self.assertNotIn("-T", step.replace("-DHYDRA", ""))   # no linker script
        self.assertNotIn("FIRMWARE_OUT", step)
        self.assertNotIn("objcopy", step)
        self.assertNotIn("link_filtered", step)

    def test_a_failure_of_the_compile_check_fails_the_build(self):
        step = SCRIPT.split('step "10b.', 1)[1].split('step "11.', 1)[0]
        self.assertIn("H757_CHECK_FAILED=1", step)
        self.assertIn("exit 1", step)


if __name__ == "__main__":
    unittest.main()
