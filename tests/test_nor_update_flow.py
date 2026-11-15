# =============================================================================
# HYDRA-UMC - tests/test_nor_update_flow.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The backup of the application slot before an update: the Python twin (tools/nor_update_flow.py) gives the same regions and the same choices as tests/test_nor_update_flow.c,
config/nor_backup.cfg holds the starting values compiled into Common/nor_update_flow.h, the two regions of the file do not overlap and fit the NOR, the switch ships off and only the
NOR file of the CM7 bootloader calls the flow behind it, the whole update (backup, staging, commit, start-up choice) survives a power cut at every operation, and the new header is in
both runners with a floor. Evidence: simulator."""
import json
import re
import sys
import tempfile
import unittest
import zlib
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


def image(seed: int, length: int) -> bytes:
    return bytes((seed + i * 7) & 0xFF for i in range(length))


def board(old: bytes = b"", require_backup: int = 1) -> nu.Board:
    cfg = small()
    cfg["require_backup"] = require_backup
    b = nu.Board(cfg)
    b.flash[: len(old)] = old
    return b


class Backup(unittest.TestCase):
    def test_a_good_application_is_saved_into_the_previous_region_only(self):
        old = image(3, 10000)
        b = board(old)
        self.assertEqual(b.backup_before_update(True, len(old)), (nu.OK, True))
        self.assertTrue(b.copy_exists("previous"))
        self.assertFalse(b.copy_exists("current"))
        start = b.regions[1]["image_offset"]
        self.assertEqual(bytes(b.nor[start : start + len(old)]), old)

    def test_a_broken_application_is_not_saved_and_the_update_goes_on(self):
        b = board()
        self.assertEqual(b.backup_before_update(False, 0), (nu.SKIPPED, True))
        self.assertEqual(b.done, 0)

    def test_a_backup_that_fails_refuses_the_update_only_when_the_file_requires_it(self):
        for require, allowed in ((1, False), (0, True)):
            b = board(image(3, 10000), require)
            b.nor_dead = True
            self.assertEqual(b.backup_before_update(True, 10000), (nu.E_BACKUP, allowed))
        too_big = board(image(3, 10000))
        self.assertEqual(too_big.backup_before_update(True, 16385), (nu.E_BACKUP, False))

    def test_a_bad_layout_cannot_make_a_board(self):
        with self.assertRaises(ValueError):
            nu.Board(small(65536))


class StartUp(unittest.TestCase):
    def setUp(self):
        self.old, self.new = image(3, 10000), image(77, 12000)
        self.old_crc, self.new_crc = zlib.crc32(self.old), zlib.crc32(self.new)

    def put(self, region, data):
        b = self.b
        b.flash[:] = b"\xff" * len(b.flash)
        b.flash[: len(data)] = data
        b.save(region, len(data))

    def test_a_good_application_is_left_alone(self):
        self.b = board()
        self.assertEqual(self.b.recover_for(True, 10000, self.old_crc), (nu.OK, nu.NONE, 0, 0))

    def test_the_current_copy_comes_back_only_when_it_is_the_image_of_the_metadata(self):
        self.b = board()
        self.put("current", self.old)
        self.b.flash[:] = b"\x00" * len(self.b.flash)
        self.assertEqual(self.b.recover_for(False, 10000, self.old_crc), (nu.OK, nu.RESTORE_CURRENT, 10000, self.old_crc))
        self.assertEqual(bytes(self.b.flash[:10000]), self.old)
        self.b.flash[:] = b"\x00" * len(self.b.flash)
        for length, crc in ((10000, self.old_crc ^ 1), (9999, self.old_crc)):  # an older or another copy never replaces the application
            self.assertEqual(self.b.recover_for(False, length, crc), (nu.E_NONE, nu.LOST, 0, 0))
        self.assertEqual(bytes(self.b.flash[:4]), b"\x00" * 4)

    def test_the_previous_application_comes_back_and_says_what_it_is(self):
        self.b = board()
        self.put("current", self.old)
        self.put("previous", self.new)
        self.b.flash[:] = b"\x00" * len(self.b.flash)
        self.assertEqual(self.b.recover_for(False, 10000, self.old_crc ^ 1), (nu.OK, nu.ROLLBACK_PREVIOUS, 12000, self.new_crc))
        self.assertEqual(bytes(self.b.flash[:12000]), self.new)

    def test_a_damaged_current_copy_gives_way_to_the_previous_one(self):
        self.b = board()
        self.put("current", self.old)
        self.put("previous", self.new)
        self.b.nor[self.b.regions[0]["image_offset"] + 100] ^= 0x10
        self.assertEqual(self.b.recover_for(False, 10000, self.old_crc)[:2], (nu.OK, nu.ROLLBACK_PREVIOUS))

    def test_nothing_to_put_back_loses_the_application(self):
        self.b = board()
        self.assertEqual(self.b.recover_for(False, 10000, self.old_crc), (nu.E_NONE, nu.LOST, 0, 0))

    def test_a_flash_that_refuses_the_image_falls_back_and_then_loses(self):
        self.b = board()
        self.put("current", self.old)
        self.put("previous", self.new)
        real = self.b.flash_write
        calls = []

        def refuse_first(offset, data):
            calls.append(offset)
            if len(calls) == 1:
                raise nu.CopyError("the flash refuses")
            real(offset, data)

        self.b.flash_write = refuse_first
        self.assertEqual(self.b.recover_for(False, 10000, self.old_crc)[:2], (nu.OK, nu.ROLLBACK_PREVIOUS))
        self.b.flash_write = lambda offset, data: (_ for _ in ()).throw(nu.CopyError("the flash refuses"))
        self.assertEqual(self.b.recover_for(False, 10000, self.old_crc)[:2], (nu.E_NONE, nu.LOST))


class PowerCuts(unittest.TestCase):
    """A cut of the power at every operation of a whole update: backup of the running application, staging of the new one, finish, commit over the flash."""

    OLD, NEW = image(3, 10000), image(77, 12000)

    def run_update(self, cut):
        b = board(self.OLD)
        b.left = cut
        state = {"finished": False}
        try:
            b.backup_before_update(True, len(self.OLD), nu.version_pack(0, 9, 4))
            b.stage_begin(len(self.NEW))
            for offset in range(0, len(self.NEW), 4096):
                b.stage_page(offset, self.NEW[offset : offset + 4096])
            b.stage_finish(len(self.NEW), zlib.crc32(self.NEW))
            state["finished"] = True
            b.restore("current")
        except nu.PowerCut:
            pass
        b.left = None
        return b, state["finished"], b.done

    def test_the_board_always_starts_with_one_of_the_two_images_whole(self):
        full, finished, total = self.run_update(None)
        self.assertTrue(finished)
        self.assertEqual(bytes(full.flash[:12000]), self.NEW)
        self.assertGreater(total, 20)
        for cut in range(total):
            b, finished, _ = self.run_update(cut)
            want, crc = (self.NEW, zlib.crc32(self.NEW)) if finished else (self.OLD, zlib.crc32(self.OLD))
            good = zlib.crc32(bytes(b.flash[: len(want)])) == crc
            result, used, length, restored_crc, version = b.recover_for_versioned(good, len(want), crc)
            self.assertEqual(result, nu.OK, cut)
            self.assertNotEqual(used, nu.LOST, cut)
            self.assertEqual(bytes(b.flash[: len(want)]), want, cut)
            if used == nu.ROLLBACK_PREVIOUS:
                self.assertEqual((length, restored_crc), (len(self.OLD), zlib.crc32(self.OLD)), cut)
                self.assertEqual(version, nu.version_pack(0, 9, 4), cut)

    def test_a_cut_during_the_backup_leaves_the_running_application_untouched(self):
        for cut in range(40):
            b = board(self.OLD)
            b.left = cut
            try:
                b.backup_before_update(True, len(self.OLD))
            except nu.PowerCut:
                pass
            self.assertEqual(bytes(b.flash[: len(self.OLD)]), self.OLD, cut)


class PreviousVersion(unittest.TestCase):
    """The version of the saved application travels in the header of the previous copy; a copy without one reads as 0.0.0."""

    V1, V2 = nu.version_pack(0, 9, 4), nu.version_pack(1, 20, 1023)

    def test_the_packing_is_the_one_of_c(self):
        self.assertEqual(nu.version_pack(0, 0, 0), 0)
        self.assertEqual(self.V1, (9 << 10) | 4)
        self.assertEqual(self.V2, (1 << 20) | (20 << 10) | 1023)
        self.assertEqual(nu.version_unpack(self.V2), (1, 20, 1023))
        for bad in ((1024, 0, 0), (0, 1024, 0), (0, 0, 1024), (-1, 0, 0)):
            self.assertEqual(nu.version_pack(*bad), 0, bad)
        self.assertEqual(nu.version_unpack(0x40000000 | self.V1), (0, 0, 0))

    def test_the_header_keeps_it_under_its_crc(self):
        import nor_backup_plan as nb

        raw = nb.header_encode(7, 100, 5, self.V1)
        self.assertEqual(nb.header_decode(raw, 200)["version"], self.V1)
        self.assertEqual(raw[16:20], self.V1.to_bytes(4, "little"))
        self.assertIsNone(nb.header_decode(raw[:17] + bytes([raw[17] ^ 1]) + raw[18:], 200))
        self.assertEqual(nb.header_decode(nb.header_encode(7, 100, 5), 200)["version"], 0)  # the first copies kept zero there

    def test_the_way_back_returns_the_version(self):
        old, new = image(3, 10000), image(77, 12000)
        b = board(old)
        self.assertEqual(b.backup_before_update(True, 10000, self.V1), (nu.OK, True))
        b.flash[:] = b"\x00" * len(b.flash)
        self.assertEqual(b.recover_for_versioned(False, 5, 6), (nu.OK, nu.ROLLBACK_PREVIOUS, 10000, zlib.crc32(old), self.V1))
        self.assertEqual(b.recover_for(False, 5, 6), (nu.OK, nu.ROLLBACK_PREVIOUS, 10000, zlib.crc32(old)))
        b.flash[:12000] = new
        self.assertEqual(b.backup_before_update(True, 12000, self.V2), (nu.OK, True))
        self.assertEqual(b.recover_for_versioned(False, 5, 6)[4], self.V2)

    def test_an_old_copy_and_the_current_copy_carry_no_version(self):
        old = image(3, 10000)
        b = board(old)
        b.save("previous", 10000)  # as the first copies were made
        b.flash[:] = b"\x00" * len(b.flash)
        self.assertEqual(b.recover_for_versioned(False, 5, 6), (nu.OK, nu.ROLLBACK_PREVIOUS, 10000, zlib.crc32(old), 0))
        self.assertEqual(nu.version_unpack(0), (0, 0, 0))
        current = board(old)
        current.save("current", 10000, self.V1)
        current.flash[:] = b"\x00" * len(current.flash)
        self.assertEqual(current.recover_for_versioned(False, 10000, zlib.crc32(old)), (nu.OK, nu.RESTORE_CURRENT, 10000, zlib.crc32(old), 0))
        self.assertEqual(board().recover_for_versioned(False, 5, 6), (nu.E_NONE, nu.LOST, 0, 0, 0))
        self.assertEqual(board(old).recover_for_versioned(True, 5, 6), (nu.OK, nu.NONE, 0, 0, 0))

    def test_a_cut_in_a_second_backup_never_mixes_the_version_with_another_image(self):
        first, second = image(3, 10000), image(77, 6000)
        probe = board(first)
        probe.backup_before_update(True, 10000, self.V1)
        probe.flash[:6000] = second
        probe.done = 0
        probe.backup_before_update(True, 6000, self.V2)
        total = probe.done
        self.assertGreater(total, 5)
        for cut in range(total + 1):
            b = board(first)
            b.backup_before_update(True, 10000, self.V1)
            b.flash[:6000] = second
            b.left = cut
            try:
                b.backup_before_update(True, 6000, self.V2)
            except nu.PowerCut:
                pass
            b.left = None
            b.flash[:] = b"\x00" * len(b.flash)
            result, _used, length, _crc, version = b.recover_for_versioned(False, 5, 6)
            if result == nu.OK:
                self.assertIn((length, version), ((10000, self.V1), (6000, self.V2)), cut)


class File(unittest.TestCase):
    def test_a_bad_file_is_refused(self):
        base = (ROOT / "config/nor_backup.cfg").read_text(encoding="utf-8").replace("\r\n", "\n")
        for good, bad in (("require_backup = 1", "require_backup = 2"), ("offset_bytes = 851968", "offset_bytes = 65536"), ("offset_bytes = 851968", "offset_bytes = 851969")):
            with tempfile.TemporaryDirectory() as folder:
                path = Path(folder) / "nor_backup.cfg"
                path.write_text(base.replace(good, bad, 1), encoding="utf-8")
                with self.assertRaises(ValueError, msg=bad):
                    nu.load_config(path)

    def test_the_switch_ships_off(self):
        self.assertIn("HYDRA_NOR_BACKUP_BEFORE_UPDATE 0", FLOW_H)
        self.assertIn("HYDRA_NOR_BACKUP_BEFORE_UPDATE", (ROOT / "src/mcu_stm32h757/CM7/boot/bootloader_common.h").read_text(encoding="utf-8"))

    def test_the_header_is_in_both_runners_and_has_a_floor(self):
        host = (ROOT / "tools/host_tests.py").read_text(encoding="utf-8")
        build = (ROOT / "tools/build_firmware.py").read_text(encoding="utf-8")
        self.assertIn("tests/test_nor_update_flow.c", host)
        self.assertIn("test_nor_update_flow", build)
        floors = json.loads((ROOT / "tools/host_coverage_floor.json").read_text(encoding="utf-8"))["modules"]
        self.assertEqual(floors["src/mcu_stm32h757/Common/nor_update_flow.h"], 100)


class Wiring(unittest.TestCase):
    def read(self, rel):
        return (ROOT / rel).read_bytes().decode("utf-8").replace("\r\n", "\n")

    def test_the_shared_start_of_an_update_compiles_the_backup_only_behind_the_switch(self):
        core = self.read("firmware_base/boot/boot_update_core_impl.h")
        self.assertIn("#if defined(HYDRA_NOR_BACKUP_BEFORE_UPDATE) && HYDRA_NOR_BACKUP_BEFORE_UPDATE\n    if (!BOOT_STAGE_BEFORE(update_total_size)) {", core)
        self.assertIn('#error "HYDRA_NOR_BACKUP_BEFORE_UPDATE needs a bootloader that defines BOOT_STAGE_BEFORE', core)
        self.assertEqual(core.count("if (!BOOT_STAGE_BEFORE("), 1)
        before_erase = core.index("BOOT_STAGE_BEFORE(update_total_size)") < core.index("BOOT_STAGE_ERASE(update_total_size")
        self.assertTrue(before_erase, "the running application is saved before the staging area is erased")
        for folder in ("src/mcu_stm32g474/boot", "src/mcu_stm32h757/CM4/boot", "src/mcu_stm32h757/CM7/boot"):
            self.assertEqual(self.read(folder + "/boot_update_core_impl.h"), core, folder)

    def test_only_the_cm7_defines_the_macro(self):
        stage = self.read("src/mcu_stm32h757/CM7/boot/boot_stage_nor.h")
        self.assertIn("#if HYDRA_NOR_BACKUP_BEFORE_UPDATE\n", stage)
        self.assertIn("#define BOOT_STAGE_BEFORE(total) ((void)(total), BackupRunningApplication())", stage)
        for folder in ("src/mcu_stm32g474/boot", "src/mcu_stm32h757/CM4/boot"):
            for path in (ROOT / folder).iterdir():
                if path.suffix in (".c", ".h") and path.name != "boot_update_core_impl.h":
                    self.assertNotIn("BOOT_STAGE_BEFORE", path.read_text(encoding="utf-8", errors="ignore"), path.name)

    def test_the_flow_is_called_by_the_cm7_nor_file_only_and_behind_the_switch(self):
        users = sorted(
            p.relative_to(ROOT).as_posix()
            for p in (ROOT / "src").rglob("*")
            if p.is_file() and p.suffix in (".c", ".h") and p.name != "nor_update_flow.h" and '#include "nor_update_flow.h"' in p.read_text(encoding="utf-8", errors="ignore")
        )
        self.assertEqual(users, ["src/mcu_stm32h757/CM7/boot/bootloader_nor.c"])
        source = self.read("src/mcu_stm32h757/CM7/boot/bootloader_nor.c")
        self.assertIn('#if HYDRA_NOR_BACKUP_BEFORE_UPDATE\n#include "nor_update_flow.h"\n#endif', source)
        for call in ("Nuf_BackupBeforeUpdate(", "Nuf_RecoverFor(", "Nuf_Defaults("):
            self.assertEqual(source.count(call), 1, call)
            before = source[: source.index(call)]
            self.assertGreater(before.rfind("#if HYDRA_NOR_BACKUP_BEFORE_UPDATE"), before.rfind("#endif // HYDRA_NOR_BACKUP_BEFORE_UPDATE"), call)

    def test_the_start_up_choice_is_the_one_of_the_python_twin(self):
        protocol = self.read("src/mcu_stm32h757/CM7/boot/bootloader_protocol.c")
        self.assertIn("if (!BootNor_RecoverAtStart(meta.size, meta.crc32, &restored_length, &restored_crc, restored_version, &rolled_back)) return 0;\n    if (rolled_back) return AdoptPreviousApplication(", protocol)
        self.assertIn("return BootNor_BackupBeforeUpdate(good, good ? meta.size : 0UL, meta.version_major, meta.version_minor, meta.version_patch);", protocol)
        # the rollback counter is not lowered by the metadata that describes the previous application
        adopt = protocol[protocol.index("static uint8_t AdoptPreviousApplication") :]
        adopt = adopt[: adopt.index("\n}\n")]
        self.assertNotIn("counter", adopt)
        self.assertIn("FirmwareMetadata_t adopted = *meta;", adopt)

    def test_the_default_build_gets_no_new_active_text(self):
        header = self.read("src/mcu_stm32h757/CM7/boot/bootloader_nor.h")
        self.assertIn("#ifndef HYDRA_NOR_BACKUP_BEFORE_UPDATE\n#define HYDRA_NOR_BACKUP_BEFORE_UPDATE 0\n#endif", header)
        for call in ("BootNor_BackupBeforeUpdate", "BootNor_RecoverAtStart"):
            self.assertIn("#if HYDRA_NOR_BACKUP_BEFORE_UPDATE", header[: header.index(call)])


if __name__ == "__main__":
    unittest.main()
