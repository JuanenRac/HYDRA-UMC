# =============================================================================
# HYDRA-UMC - tests/test_nor_update_flow.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The backup of the application slot before an update: the Python twin (tools/nor_update_flow.py) gives the same regions and the same choices as tests/test_nor_update_flow.c,
config/nor_backup.cfg holds the starting values compiled into Common/nor_update_flow.h, the two regions of the file do not overlap and fit the NOR, the switch ships off and nothing
in the bootloader calls the flow yet, and the new header is in both runners with a floor. Evidence: simulator."""
import json
import re
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import nor_update_flow as nu  # noqa: E402

FLOW_H = (ROOT / "src/mcu_stm32h757/Common/nor_update_flow.h").read_text(encoding="utf-8")


def small(previous_offset=131072, slot=16384, size=1024 * 1024):
    side = {"nor_offset": 0, "nor_size": size, "slot_bytes": slot}
    return {"current": dict(side), "previous": dict(side, nor_offset=previous_offset), "require_backup": 1}


class Regions(unittest.TestCase):
    def test_the_file_has_two_regions_that_touch_and_fit(self):
        cfg = nu.load_config()
        current, previous = nu.plan(cfg)
        self.assertEqual(previous["header_a"], current["end"])
        self.assertLessEqual(previous["end"], cfg["current"]["nor_size"])
        self.assertEqual(cfg["require_backup"], 1)

    def test_the_file_is_what_the_header_compiles_in(self):
        cfg = nu.load_config()
        self.assertEqual(cfg["previous"]["nor_offset"], int(re.search(r"NUF_DEFAULT_PREVIOUS_OFFSET (\d+)UL", FLOW_H).group(1)))

    def test_the_refusals_are_the_same_as_in_c(self):
        nu.plan(small())
        nu.plan(dict(small(0), current=dict(small()["current"], nor_offset=131072)))  # the previous region may come first
        for bad in (small(65536), small(0), small(3), small(1024 * 1024 - 65536)):
            with self.assertRaises(ValueError):
                nu.plan(bad)
        different = small()
        different["previous"]["slot_bytes"] = 16384 - 4096
        with self.assertRaises(ValueError):
            nu.plan(different)


class Choices(unittest.TestCase):
    def test_decide_is_the_table_of_the_c_test(self):
        self.assertEqual((nu.decide(True, True, True), nu.decide(True, False, False)), (nu.NONE, nu.NONE))
        self.assertEqual((nu.decide(False, True, False), nu.decide(False, True, True)), (nu.RESTORE_CURRENT, nu.RESTORE_CURRENT))
        self.assertEqual(nu.decide(False, False, True), nu.ROLLBACK_PREVIOUS)
        self.assertEqual(nu.decide(False, False, False), nu.LOST)
        self.assertEqual((nu.NONE, nu.RESTORE_CURRENT, nu.ROLLBACK_PREVIOUS, nu.LOST), tuple(int(re.search(rf"NUF_ACT_{n} (\d)", FLOW_H).group(1)) for n in ("NONE", "RESTORE_CURRENT", "ROLLBACK_PREVIOUS", "LOST")))

    def test_the_update_gate(self):
        self.assertTrue(nu.update_allowed(False, False, 1))  # a broken application: the update is the repair
        self.assertTrue(nu.update_allowed(True, True, 1))
        self.assertFalse(nu.update_allowed(True, False, 1))  # required backup that failed
        self.assertTrue(nu.update_allowed(True, False, 0))  # optional backup that failed


class File(unittest.TestCase):
    def test_a_bad_file_is_refused(self):
        base = (ROOT / "config/nor_backup.cfg").read_text(encoding="utf-8").replace("\r\n", "\n")
        for good, bad in (("require_backup = 1", "require_backup = 2"), ("offset_bytes = 851968", "offset_bytes = 65536"), ("offset_bytes = 851968", "offset_bytes = 851969")):
            with tempfile.TemporaryDirectory() as folder:
                path = Path(folder) / "nor_backup.cfg"
                path.write_text(base.replace(good, bad, 1), encoding="utf-8")
                with self.assertRaises(ValueError, msg=bad):
                    nu.load_config(path)

    def test_the_switch_ships_off_and_the_bootloader_does_not_call_the_flow(self):
        self.assertIn("HYDRA_NOR_BACKUP_BEFORE_UPDATE 0", FLOW_H)
        self.assertIn("HYDRA_NOR_BACKUP_BEFORE_UPDATE", (ROOT / "src/mcu_stm32h757/CM7/boot/bootloader_common.h").read_text(encoding="utf-8"))
        users = [
            p
            for p in (ROOT / "src").rglob("*")
            if p.is_file() and p.suffix in (".c", ".h") and p.name != "nor_update_flow.h" and '#include "nor_update_flow.h"' in p.read_text(encoding="utf-8", errors="ignore")
        ]
        self.assertEqual(users, [])

    def test_the_header_is_in_both_runners_and_has_a_floor(self):
        host = (ROOT / "tools/host_tests.py").read_text(encoding="utf-8")
        build = (ROOT / "tools/build_firmware.py").read_text(encoding="utf-8")
        self.assertIn("tests/test_nor_update_flow.c", host)
        self.assertIn("test_nor_update_flow", build)
        floors = json.loads((ROOT / "tools/host_coverage_floor.json").read_text(encoding="utf-8"))["modules"]
        self.assertEqual(floors["src/mcu_stm32h757/Common/nor_update_flow.h"], 100)


if __name__ == "__main__":
    unittest.main()
