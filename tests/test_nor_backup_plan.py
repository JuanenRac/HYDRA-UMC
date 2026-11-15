# =============================================================================
# HYDRA-UMC - tests/test_nor_backup_plan.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The backup copy of the CM7 application in the NOR flash and the linker scripts made from the profile: the Python twin (tools/nor_backup_plan.py) makes the same layout and header as
tests/test_nor_backup_plan.c, config/nor_backup.cfg is the same as the starting values compiled into Common/nor_backup_plan.h and as `app_backup_cm7` of the profile, the linker scripts are what
tools/linker_memory_gen.py makes from the profile (the default ones keep 384 KB, the 768 KB script is the H757 slot), a slot longer than the internal backup needs its copy in the NOR, and the
new sources are in both runners with floors. Evidence: simulator."""
from __future__ import annotations

import json
import re
import sys
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import linker_memory_gen as lg  # noqa: E402
import nor_backup_plan as nb  # noqa: E402
import platform_profile as pp  # noqa: E402

PLAN_H = (ROOT / "src/mcu_stm32h757/Common/nor_backup_plan.h").read_text(encoding="utf-8")


def define(name: str) -> int:
    match = re.search(rf"#define {name} (\d+|0x[0-9A-Fa-f]+)UL", PLAN_H)
    assert match, name
    return int(match.group(1), 0)


class NorBackupTwin(unittest.TestCase):
    def test_the_check_value_of_the_crc(self):
        self.assertEqual(zlib.crc32(b"123456789"), 0xCBF43926)

    def test_the_layout_of_the_defaults(self):
        layout = nb.plan(nb.load_config())
        self.assertEqual(layout, {"header_a": 0, "header_b": 4096, "image_offset": 65536, "image_bytes_max": 786432, "end": 65536 + 786432})

    def test_the_refusals_are_those_of_the_c_code(self):
        base = nb.load_config()
        for change in ({"nor_offset": 4096}, {"slot_bytes": 786432 + 100}, {"nor_offset": 16777216 - 65536}, {"slot_bytes": 0}, {"nor_size": 0}, {"nor_offset": 2 * 16777216}):
            with self.assertRaises(ValueError, msg=str(change)):
                nb.plan({**base, **change})
        self.assertEqual(nb.plan({**base, "nor_size": 65536 + 786432})["end"], 65536 + 786432)
        self.assertEqual(nb.plan({**base, "slot_bytes": 786432 - 4096})["end"], 65536 + 786432)

    def test_the_header_bytes_are_the_literal_ones_of_the_c_test(self):
        raw = nb.header_encode(5, 0x1000, 0xDEADBEEF)
        self.assertEqual(raw[:4], b"NORB")
        self.assertEqual(raw[4:8], (5).to_bytes(4, "little"))
        self.assertEqual(raw[8:12], (0x1000).to_bytes(4, "little"))
        self.assertEqual(raw[12:16], (0xDEADBEEF).to_bytes(4, "little"))
        self.assertEqual(nb.header_decode(raw, 0x2000), {"sequence": 5, "length": 0x1000, "crc32": 0xDEADBEEF})
        self.assertIsNone(nb.header_decode(raw, 0x800))
        self.assertIsNone(nb.header_decode(bytes([raw[0] ^ 1]) + raw[1:], 0x2000))
        self.assertIsNone(nb.header_decode(raw[:5] + bytes([raw[5] ^ 1]) + raw[6:], 0x2000))
        self.assertIsNone(nb.header_decode(nb.header_encode(5, 0, 0), 0x2000))

    def test_the_newer_header_wins_even_when_the_sequence_wraps(self):
        a, b = {"sequence": 0xFFFFFFFF}, {"sequence": 0}
        self.assertEqual(nb.newest(a, b), (b, 1))
        self.assertEqual(nb.newest({"sequence": 3}, {"sequence": 2}), ({"sequence": 3}, 0))
        self.assertEqual(nb.newest(None, b), (b, 1))
        self.assertEqual(nb.newest(a, None), (a, 0))
        self.assertIsNone(nb.newest(None, None))


class SourcesAgree(unittest.TestCase):
    def test_the_file_is_the_starting_values_of_the_header_and_of_the_profile(self):
        cfg = nb.load_config()
        self.assertEqual((cfg["nor_offset"], cfg["nor_size"], cfg["slot_bytes"]), (define("NORB_DEFAULT_NOR_OFFSET"), define("NORB_DEFAULT_NOR_SIZE"), define("NORB_DEFAULT_SLOT_BYTES")))
        profile = pp.profile("h757_target")
        backup = profile["memory_map"]["app_backup_cm7"]
        slot = next(r for r in profile["memory_map"]["regions"] if r["name"] == "FLASH_APP_CM7")
        self.assertEqual((backup["offset_bytes"], backup["length_bytes"]), (cfg["nor_offset"], cfg["slot_bytes"]))
        self.assertEqual(slot["length"], cfg["slot_bytes"])
        self.assertEqual((define("NORB_SECTOR"), define("NORB_BLOCK"), define("NORB_HEADER_BYTES")), (nb.SECTOR, nb.BLOCK, nb.HEADER_BYTES))

    def test_the_switch_ships_off_and_nothing_in_the_firmware_calls_the_copy_yet(self):
        text = (ROOT / "config/nor_backup.cfg").read_text(encoding="utf-8")
        self.assertIn("HYDRA_NOR_BACKUP 0", text)
        users = [
            p
            for p in (ROOT / "src").rglob("*")
            if p.is_file() and p.suffix in (".c", ".h") and p.name not in {"nor_backup_plan.h", "nor_backup_glue.h"} and re.search(r'#include "nor_backup_(plan|glue)\.h"', p.read_text(encoding="utf-8", errors="ignore"))
        ]
        self.assertEqual(users, [])

    def test_the_memory_map_of_a_long_slot_needs_its_copy_in_the_nor(self):
        document = pp.load()
        self.assertEqual(pp.memory_problems("h757_target", document["profiles"]["h757_target"]), [])
        h757 = json.loads(json.dumps(document["profiles"]["h757_target"]))
        memory = h757["memory_map"]
        backup = memory.pop("app_backup_cm7")
        self.assertTrue(any("nowhere" in line for line in pp.backup_problems("x", h757)))
        memory["app_backup_cm7"] = {**backup, "where": "ELSEWHERE"}
        self.assertTrue(any("not a memory" in line for line in pp.backup_problems("x", h757)))
        memory["app_backup_cm7"] = {**backup, "length_bytes": 4096}
        self.assertTrue(any("smaller" in line for line in pp.backup_problems("x", h757)))
        memory["app_backup_cm7"] = {**backup, "offset_bytes": 4096}
        self.assertTrue(any("erase units" in line for line in pp.backup_problems("x", h757)))
        memory["app_backup_cm7"] = {**backup, "offset_bytes": 16777216 - 65536}
        self.assertTrue(any("does not fit" in line for line in pp.backup_problems("x", h757)))
        self.assertEqual(pp.backup_problems("h745_current", document["profiles"]["h745_current"]), [])


class LinkerScripts(unittest.TestCase):
    def test_every_script_is_what_the_profile_says(self):
        self.assertEqual(lg.problems(), [])

    def test_the_default_scripts_keep_384k_and_the_h757_script_has_768k(self):
        text = {t[0].name: t[0].read_text(encoding="utf-8").replace("\r\n", "\n") for t in lg.TARGETS}
        self.assertIn("FLASH_APP (rx) : ORIGIN = 0x08040000, LENGTH = 384K", text["STM32H757XIHx_CM7_APP.ld"])
        self.assertIn("FLASH_APP (rx) : ORIGIN = 0x08140000, LENGTH = 384K", text["STM32H757XIHx_CM4_APP.ld"])
        self.assertIn("FLASH_APP (rx) : ORIGIN = 0x08040000, LENGTH = 768K", text["STM32H757XIHx_CM7_APP_768K.ld"])
        derived = text["STM32H757XIHx_CM7_APP.ld"].replace("h745_current", "h757_target").replace("LENGTH = 384K\n  RAM", "LENGTH = 768K\n  RAM", 1)
        self.assertEqual(derived, text["STM32H757XIHx_CM7_APP_768K.ld"])

    def test_the_default_build_does_not_use_the_long_script_or_the_long_size(self):
        build = (ROOT / "tools/build_firmware.py").read_text(encoding="utf-8")
        self.assertNotIn("APP_768K", build)
        common = (ROOT / "src/mcu_stm32h757/CM7/boot/bootloader_common.h").read_text(encoding="utf-8")
        self.assertIn("#define APP_MAX_SIZE         (384UL * 1024UL)", common)

    def test_a_script_with_a_changed_block_is_found_and_a_script_without_block_is_refused(self):
        document = pp.load()
        document["profiles"]["h745_current"]["memory_map"]["regions"][1]["length"] = 100 * 1024
        self.assertTrue(any("CM7_APP.ld" in line for line in lg.problems(document)))
        with self.assertRaises(ValueError):
            lg.render((lg.TARGETS[0][0], "h745_current", "CM7", ROOT / "tools" / "linker_memory_gen.py"))

    def test_the_command_line_checks(self):
        self.assertEqual(lg.main(["--check"]), 0)


class Registered(unittest.TestCase):
    def test_the_new_sources_are_in_both_runners_and_have_floors(self):
        host = (ROOT / "tools/host_tests.py").read_text(encoding="utf-8")
        build = (ROOT / "tools/build_firmware.py").read_text(encoding="utf-8")
        self.assertIn("tests/test_nor_backup_plan.c", host)
        self.assertIn("test_nor_backup_plan", build)
        floors = json.loads((ROOT / "tools/host_coverage_floor.json").read_text(encoding="utf-8"))["modules"]
        self.assertEqual(floors["src/mcu_stm32h757/Common/nor_backup_plan.h"], 100)


if __name__ == "__main__":
    unittest.main()
