# =============================================================================
# HYDRA-UMC - tests/test_long_slot_plan.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The long-slot variant of the CM7 firmware (tools/long_slot_plan.py, config/long_slot.cfg, step 10c of tools/build_firmware.py): the file is read and refused when it is wrong, the sources, the
two linker scripts, the profile and the file agree, the default images cannot change (the default slot stays 384 KB and the variant is off unless the environment asks for it), the names of the
long images can never be taken for the default set, and the link check compares the application with the interface budget. Evidence: simulator (sizes of a build, nothing ran on a board)."""
from __future__ import annotations

import re
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import build_firmware  # noqa: E402
import h745_invariance as inv  # noqa: E402
import long_slot_plan as ls  # noqa: E402


def with_cfg(text_change: tuple[str, str]) -> dict:
    """The configuration of the repository with one text replaced, loaded from a scratch file."""
    original = (ROOT / "config" / "long_slot.cfg").read_text(encoding="utf-8")
    assert text_change[0] in original, text_change[0]
    folder = Path(tempfile.mkdtemp(prefix="hydra-long-slot-"))
    try:
        path = folder / "long_slot.cfg"
        path.write_text(original.replace(*text_change), encoding="utf-8")
        return ls.load_config(path)
    finally:
        shutil.rmtree(folder, ignore_errors=True)


class ConfigTests(unittest.TestCase):
    def test_the_file_of_the_repository_loads(self) -> None:
        cfg = ls.load_config()
        self.assertEqual(cfg["output_dir"], "long_slot")
        self.assertEqual(cfg["image_suffix"], "LONG_SLOT")
        self.assertEqual(cfg["free_percent"], 10)
        for required in ls.REQUIRED_DEFINES:
            self.assertIn(required, cfg["bootloader_defines"])

    def test_the_flags_are_the_definitions_with_a_dash_d(self) -> None:
        self.assertEqual(ls.compile_flags({"bootloader_defines": ["HYDRA_A=1", "HYDRA_B"]}), ["-DHYDRA_A=1", "-DHYDRA_B"])

    def test_the_file_is_refused_when_it_is_wrong(self) -> None:
        for change in (
            ("output_dir = long_slot", "output_dir = ../outside"),
            ("image_suffix = LONG_SLOT", "image_suffix = long slot"),
            ("HYDRA_NOR_SPI5=1", "HYDRA_NOR_SPI5=0"),
            ("HYDRA_NOR_BACKUP=1 ", ""),
            ("HYDRA_PLATFORM_H757 ", "HYDRA_PLATFORM_H757 ; rm "),
            ("free_percent = 10", "free_percent = 100"),
            ("free_percent = 10", "free_percent = -1"),
        ):
            with self.assertRaises(ValueError, msg=str(change)):
                with_cfg(change)

    def test_a_percentage_inside_the_range_is_accepted(self) -> None:
        self.assertEqual(with_cfg(("free_percent = 10", "free_percent = 0"))["free_percent"], 0)
        self.assertEqual(with_cfg(("free_percent = 10", "free_percent = 99.5"))["free_percent"], 99.5)


class AgreementTests(unittest.TestCase):
    def test_the_sources_the_scripts_and_the_profile_agree(self) -> None:
        self.assertEqual(ls.check_sources(), [])

    def test_the_slot_is_768_kib_and_the_default_script_is_not(self) -> None:
        self.assertEqual(ls.slot_bytes(), 768 * 1024)
        text = (ROOT / "src/mcu_stm32h757/CM7/STM32H757XIHx_CM7_APP.ld").read_text(encoding="utf-8")
        self.assertIn("LENGTH = 384K", text)

    def test_the_check_notices_a_changed_default_slot(self) -> None:
        folder = Path(tempfile.mkdtemp(prefix="hydra-long-slot-root-"))
        self.addCleanup(shutil.rmtree, folder, ignore_errors=True)
        for relative in (
            "config/long_slot.cfg",
            "config/nor_backup.cfg",
            "tools/bootloader_size.json",
            "src/mcu_stm32h757/CM7/boot/bootloader_common.h",
            "src/mcu_stm32h757/CM7/STM32H757XIHx_CM7_APP.ld",
            "src/mcu_stm32h757/CM7/STM32H757XIHx_CM7_APP_768K.ld",
        ):
            (folder / relative).parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / relative, folder / relative)
        self.assertEqual(ls.check_sources(folder), [])
        common = folder / "src/mcu_stm32h757/CM7/boot/bootloader_common.h"
        text = common.read_text(encoding="utf-8")
        common.write_text(text.replace("(384UL * 1024UL)", "(768UL * 1024UL)"), encoding="utf-8")
        self.assertTrue(any("no longer 384 KB" in p for p in ls.check_sources(folder)))
        common.write_text(text.replace('#error "HYDRA_APP_SLOT_768K needs', '#warning "HYDRA_APP_SLOT_768K needs'), encoding="utf-8")
        self.assertTrue(any("no longer refuses" in p for p in ls.check_sources(folder)))
        common.write_text(text, encoding="utf-8")
        script = folder / "src/mcu_stm32h757/CM7/STM32H757XIHx_CM7_APP.ld"
        script_text = script.read_text(encoding="utf-8")
        script.write_text(script_text.replace("LENGTH = 384K", "LENGTH = 768K"), encoding="utf-8")
        self.assertTrue(any("default CM7 application script" in p for p in ls.check_sources(folder)))
        script.write_text(script_text, encoding="utf-8")
        long_script = folder / "src/mcu_stm32h757/CM7/STM32H757XIHx_CM7_APP_768K.ld"
        long_script.write_text(long_script.read_text(encoding="utf-8").replace("LENGTH = 768K", "LENGTH = 700K"), encoding="utf-8")
        problems = ls.check_sources(folder)
        self.assertTrue(any("not 768 KiB" in p for p in problems))
        self.assertTrue(any("another slot size" in p for p in problems))

    def test_the_default_images_are_built_without_the_variant(self) -> None:
        # the variant is off unless the environment asks, and the step is the only place that mentions it
        source = (ROOT / "tools" / "build_firmware.py").read_text(encoding="utf-8")
        self.assertEqual(len(re.findall(r'HYDRA_LONG_SLOT", "0"\) == "1"', source)), 1)
        self.assertNotIn("HYDRA_APP_SLOT_768K=1", source.split("def protocol_nor_commands")[0].split("def bootloader_cases")[0])


class NamesTests(unittest.TestCase):
    def test_a_long_image_is_not_taken_for_a_default_image(self) -> None:
        cfg = ls.load_config()
        for component in ("BOOTLOADER", "APP"):
            name = ls.image_name(component, "v0.1.7", cfg) + ".bin"
            self.assertIsNotNone(inv.IMAGE_NAME.fullmatch(name))
            matched = inv.IMAGE_NAME.fullmatch(name)
            assert matched is not None
            self.assertNotIn(matched.group("component"), inv.COMPONENTS)  # a stray copy in firmware/ would be reported as new, never mixed up


class LinkCheckTests(unittest.TestCase):
    BOOT = {"flash_free": 20000, "flash_region": 131072}

    def test_a_small_application_with_the_interface_fits(self) -> None:
        ok, lines = ls.link_check(self.BOOT, 40000, 786432, 352907, 10, 4096)
        self.assertTrue(ok)
        self.assertTrue(all(line.startswith("OK") for line in lines))

    def test_an_application_that_leaves_less_than_the_share_free_fails(self) -> None:
        limit = int(786432 * 0.9)
        ok, lines = ls.link_check(self.BOOT, limit - 352907 + 1, 786432, 352907, 10, 4096)
        self.assertFalse(ok)
        self.assertTrue(lines[-1].startswith("FAIL") and "DOES NOT FIT" in lines[-1])
        ok, _ = ls.link_check(self.BOOT, limit - 352907, 786432, 352907, 10, 4096)
        self.assertTrue(ok)  # exactly on the limit still fits

    def test_a_bootloader_without_its_margin_fails(self) -> None:
        ok, lines = ls.link_check({"flash_free": 4095, "flash_region": 131072}, 1000, 786432, 352907, 10, 4096)
        self.assertFalse(ok)
        self.assertTrue(lines[0].startswith("FAIL"))
        ok, _ = ls.link_check({"flash_free": 4096, "flash_region": 131072}, 1000, 786432, 352907, 10, 4096)
        self.assertTrue(ok)

    def test_the_size_tool_output_is_read(self) -> None:
        fake = mock.Mock(stdout="   text    data     bss     dec     hex filename\n  12345     100     200   12645    3165 x.elf\n")
        with mock.patch.object(ls.subprocess, "run", return_value=fake):
            self.assertEqual(ls.read_size(Path("x.elf")), (12345, 100, 200))


class CommandLineTests(unittest.TestCase):
    def test_check_and_plan_run(self) -> None:
        self.assertEqual(ls.main(["--check"]), 0)
        self.assertEqual(ls.main(["--plan"]), 0)
        self.assertEqual(ls.main([]), 0)

    def test_link_reads_both_images_and_gives_the_verdict(self) -> None:
        sizes = {"boot.elf": (60000, 100, 300), "app.elf": (50000, 200, 400)}
        with mock.patch.object(ls, "read_size", side_effect=lambda elf, *a: sizes[Path(elf).name]):
            self.assertEqual(ls.main(["--link", "boot.elf", "app.elf"]), 0)
        sizes["app.elf"] = (500000, 200, 400)
        with mock.patch.object(ls, "read_size", side_effect=lambda elf, *a: sizes[Path(elf).name]):
            self.assertEqual(ls.main(["--link", "boot.elf", "app.elf"]), 1)

    def test_check_fails_when_a_problem_is_found(self) -> None:
        with mock.patch.object(ls, "check_sources", return_value=["a problem"]):
            self.assertEqual(ls.main(["--check"]), 1)


class BuildStepTests(unittest.TestCase):
    def test_the_variant_is_a_step_of_the_build_and_only_with_the_switch(self) -> None:
        self.assertTrue(hasattr(build_firmware.Stage2, "step10c"))
        calls: list[str] = []

        class Probe(build_firmware.Stage2):
            def __getattribute__(self, name):  # record the steps that run, run none
                if name.startswith("step") or name == "makedirs":
                    calls.append(name)
                    return lambda *a, **k: None
                return super().__getattribute__(name)

        for switch, expected in (("1", True), ("0", False), (None, False)):
            calls.clear()
            env = {} if switch is None else {"HYDRA_LONG_SLOT": switch}
            build = build_firmware.Build("/nonexistent", build_firmware.Report(out=lambda _: None), env=env)
            stage = Probe.__new__(Probe)
            object.__setattr__(stage, "b", build)
            object.__setattr__(stage, "out", "/nonexistent/out")
            object.__setattr__(stage, "final", "/nonexistent/final")
            object.__setattr__(stage, "target", "h745")
            with mock.patch.object(build_firmware, "sh_glob", return_value=[]):
                build_firmware.Stage2.run_all(stage)
            self.assertEqual("step10c" in calls, expected, switch)


if __name__ == "__main__":
    unittest.main()
