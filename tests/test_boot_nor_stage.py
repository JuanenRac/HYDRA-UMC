# =============================================================================
# HYDRA-UMC - tests/test_boot_nor_stage.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The staging of an update in the NOR flash by the bootloader of the CM7 and the pins of SPI5: the Python twin (tools/boot_nor_stage.py) follows the same steps and refusals as
tests/test_boot_nor_stage.c and tests/test_nor_spi5_pins.c, config/nor_spi5.cfg is the same as the starting values compiled into Common/nor_spi5_pins.h, the shared bootloader files stay
byte-identical to the default build (the new lines sit behind HYDRA_NOR_BACKUP), and the new sources are in both runners with floors. Evidence: simulator."""
from __future__ import annotations

import json
import re
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import boot_nor_stage as bns  # noqa: E402
import nor_backup_plan as nb  # noqa: E402

PINS_H = (ROOT / "src/mcu_stm32h757/Common/nor_spi5_pins.h").read_text(encoding="utf-8")
SLOT = 16 * 1024
NOR = 1024 * 1024
PAGE = 2048


def define(name: str) -> int:
    match = re.search(rf"#define {name} (\d+)U", PINS_H)
    assert match, name
    return int(match.group(1))


def make() -> bns.Stage:
    cfg = {"nor_offset": 65536, "nor_size": NOR, "slot_bytes": SLOT}
    return bns.Stage(bytearray(b"\xff" * NOR), bytearray(b"\xff" * SLOT), cfg)


def image(seed: int, length: int = SLOT) -> bytes:
    return bytes((seed + i * 7 + (i >> 8)) & 0xFF for i in range(length))


def stage_all(stage: bns.Stage, img: bytes, total: int) -> int:
    crc = zlib.crc32(img[:total])
    stage.begin(total)
    for index in range(-(-total // PAGE)):
        chunk = img[index * PAGE : min(total, (index + 1) * PAGE)]
        stage.page(index, PAGE, chunk.ljust(PAGE, b"\xff"))
    stage.finish(total, crc)
    return crc


class Pins(unittest.TestCase):
    def test_the_file_is_the_starting_values_of_the_header(self):
        pins = bns.load_pins()
        self.assertEqual(pins["cs_port"], define("NORS5_CS_PORT"))
        self.assertEqual(pins["cs_pin"], define("NORS5_CS_PIN"))
        self.assertEqual(pins["bus_port"], define("NORS5_BUS_PORT"))
        self.assertEqual((pins["sck_pin"], pins["miso_pin"], pins["mosi_pin"]), (define("NORS5_SCK_PIN"), define("NORS5_MISO_PIN"), define("NORS5_MOSI_PIN")))
        self.assertEqual((pins["alternate"], pins["timeout_ms"]), (define("NORS5_ALTERNATE"), define("NORS5_TIMEOUT_MS")))
        self.assertEqual(bns.bus_mask(pins), (1 << 7) | (1 << 8) | (1 << 9))

    def test_a_pin_is_a_port_letter_and_a_number(self):
        self.assertEqual(bns.parse_pin("F6"), (5, 6))
        self.assertEqual(bns.parse_pin(" k15 "), (10, 15))
        for bad in ("", "F", "6", "L1", "F16", "F-1", "FF1", "1F"):
            with self.assertRaises(ValueError, msg=bad):
                bns.parse_pin(bad)

    def test_every_wrong_set_is_refused_as_the_c_code_refuses_it(self):
        base = bns.load_pins()
        for change, text in (
            ({"cs_port": 11}, "port"),
            ({"bus_port": 11}, "port"),
            ({"cs_pin": 16}, "above 15"),
            ({"alternate": 16}, "above 15"),
            ({"miso_pin": base["sck_pin"]}, "same"),
            ({"mosi_pin": base["sck_pin"]}, "same"),
            ({"cs_pin": base["sck_pin"]}, "chip select"),
            ({"timeout_ms": 0}, "timeout"),
            ({"timeout_ms": 1001}, "timeout"),
        ):
            problems = bns.pin_problems({**base, **change})
            self.assertTrue(any(text in line for line in problems), (change, problems))
        self.assertEqual(bns.pin_problems({**base, "cs_port": 0, "cs_pin": base["sck_pin"]}), [])
        self.assertEqual(bns.pin_problems({**base, "timeout_ms": 1000}), [])

    def test_a_file_with_the_bus_on_two_ports_or_a_bad_pin_does_not_load(self):
        text = (ROOT / "config/nor_spi5.cfg").read_text(encoding="utf-8")
        for old, new in (("miso = F8", "miso = G8"), ("mosi = F9", "mosi = F7"), ("cs = F6", "cs = F7"), ("transfer_timeout_ms = 20", "transfer_timeout_ms = 0")):
            with tempfile.TemporaryDirectory() as folder:
                path = Path(folder) / "nor_spi5.cfg"
                path.write_text(text.replace(old, new), encoding="utf-8")
                with self.assertRaises(ValueError, msg=new):
                    bns.load_pins(path)


class StageTwin(unittest.TestCase):
    def test_a_staged_update_commits_and_is_the_copy(self):
        stage = make()
        a = image(0x11)
        crc = stage_all(stage, a, 5000)
        self.assertTrue(stage.copy_is(5000, crc))
        self.assertFalse(stage.copy_is(5000, crc ^ 1))
        self.assertFalse(stage.copy_is(4999, crc))
        stage.commit(5000, crc)
        self.assertEqual(bytes(stage.flash[:5000]), a[:5000])
        self.assertEqual(bytes(stage.flash[5000:]), b"\xff" * (SLOT - 5000))

    def test_the_commit_of_another_image_is_refused_and_the_flash_is_left_alone(self):
        stage = make()
        crc = stage_all(stage, image(0x11), 5000)
        with self.assertRaises(ValueError):
            stage.commit(5000, crc ^ 1)
        self.assertEqual(bytes(stage.flash), b"\xff" * SLOT)

    def test_an_incomplete_image_is_not_finished(self):
        stage = make()
        a = image(0x11)
        stage.begin(5000)
        stage.page(0, PAGE, a[:PAGE])
        with self.assertRaises(ValueError):
            stage.finish(5000, zlib.crc32(a[:5000]))
        self.assertIsNone(stage.headers())

    def test_the_sizes_the_slot_cannot_hold_are_refused(self):
        stage = make()
        for total in (0, SLOT + 1):
            with self.assertRaises(ValueError):
                stage.begin(total)
            with self.assertRaises(ValueError):
                stage.finish(total, 0)
        stage.begin(SLOT)
        stage.page(SLOT // PAGE - 1, PAGE, b"\x00" * PAGE)
        with self.assertRaises(ValueError):
            stage.page(SLOT // PAGE, PAGE, b"\x00" * PAGE)
        with self.assertRaises(ValueError):
            stage.page(0, PAGE, b"")

    def test_the_second_update_goes_into_the_other_header(self):
        stage = make()
        stage_all(stage, image(0x11), 5000)
        first = stage.headers()
        stage_all(stage, image(0x77), 6000)
        second = stage.headers()
        self.assertEqual((first[1], second[1]), (0, 1))
        self.assertEqual(second[0]["sequence"], first[0]["sequence"] + 1)
        stage_all(stage, image(0x33), 4000)
        self.assertEqual(stage.headers()[1], 0)

    def test_the_rule_at_boot(self):
        stage = make()
        a = image(0x11)
        self.assertEqual(stage.recover(True, 5000, 0), bns.RECOVER_NONE)
        self.assertEqual(stage.recover(False, 5000, 0), bns.RECOVER_LOST)
        crc = stage_all(stage, a, 5000)
        stage.commit(5000, crc)
        stage.flash[10] ^= 0x40
        self.assertEqual(stage.recover(True, 5000, crc), bns.RECOVER_NONE)
        self.assertNotEqual(stage.flash[10], a[10])
        self.assertEqual(stage.recover(False, 5000, crc), bns.RECOVER_DONE)
        self.assertEqual(bytes(stage.flash[:5000]), a[:5000])
        # a copy that is not the image of the metadata is not used
        stage.flash[10] ^= 0x40
        self.assertEqual(stage.recover(False, 5000, crc ^ 1), bns.RECOVER_LOST)
        self.assertEqual(stage.recover(False, 5001, crc), bns.RECOVER_LOST)
        # nor is a copy whose image was damaged after the header
        stage.nor[stage.layout["image_offset"] + 100] = 0
        self.assertEqual(stage.recover(False, 5000, crc), bns.RECOVER_LOST)
        with self.assertRaises(ValueError):
            stage.commit(5000, crc)


class SharedFilesAndRegistration(unittest.TestCase):
    def test_the_shared_text_adds_the_nor_only_behind_the_switch(self):
        core = (ROOT / "firmware_base/boot/boot_update_core_impl.h").read_text(encoding="utf-8")
        self.assertRegex(core, r"#if defined\(HYDRA_NOR_BACKUP\) && HYDRA_NOR_BACKUP\s+#include \"boot_stage_nor.h\"\s+#else\s+#define BOOT_STAGE_ERASE\(total, pages\) Flash_ErasePages\(BACKUP_APP_ADDR, \(pages\)\)")
        self.assertIn("uint32_t page_addr = BACKUP_APP_ADDR + (current_page_index * FLASH_PAGE_SIZE);", core)
        crypto = (ROOT / "firmware_base/boot/bootloader_crypto.c").read_text(encoding="utf-8")
        self.assertIn("#if defined(HYDRA_NOR_BACKUP) && HYDRA_NOR_BACKUP\n_Static_assert(sizeof(SHA256_CTX) <= sizeof(Sha256Stream_t)", crypto.replace("\r\n", "\n"))
        header = (ROOT / "firmware_base/boot/bootloader_crypto.h").read_text(encoding="utf-8").replace("\r\n", "\n")
        self.assertIn("#if defined(HYDRA_NOR_BACKUP) && HYDRA_NOR_BACKUP\ntypedef struct { uint64_t opaque[16]; } Sha256Stream_t;", header)

    def test_the_protocol_source_calls_the_nor_only_behind_the_switch(self):
        source = (ROOT / "src/mcu_stm32h757/CM7/boot/bootloader_protocol.c").read_text(encoding="utf-8").replace("\r\n", "\n")
        for call in ("BootNor_Commit(", "BootNor_Digest(", "BootNor_FirstWords(", "BootNor_Finish(", "BootNor_RestoreIfLost("):
            for match in re.finditer(re.escape(call), source):
                before = source[: match.start()]
                last_if = max(before.rfind("#if HYDRA_NOR_BACKUP"), -1)
                last_else = max(before.rfind("#else", last_if), before.rfind("#endif", last_if))
                self.assertGreater(last_if, -1, call)
                self.assertLess(last_else, last_if, call)

    def test_the_common_text_keeps_the_nor_files_in_the_cm7_boot_folder_only(self):
        for folder in ("src/mcu_stm32g474/boot", "src/mcu_stm32h757/CM4/boot"):
            self.assertFalse((ROOT / folder / "boot_stage_nor.h").exists())
        for name in ("bootloader_nor.c", "bootloader_nor.h", "boot_stage_nor.h"):
            self.assertTrue((ROOT / "src/mcu_stm32h757/CM7/boot" / name).is_file())

    def test_the_new_sources_are_in_both_runners_and_have_floors(self):
        host = (ROOT / "tools/host_tests.py").read_text(encoding="utf-8")
        build = (ROOT / "tools/build_firmware.py").read_text(encoding="utf-8")
        for test in ("tests/test_boot_nor_stage.c", "tests/test_nor_spi5_pins.c"):
            self.assertIn(test, host)
        for token in ("test_boot_nor_stage", "test_nor_spi5_pins", "protocol_nor_commands", "bootloader_nor.c"):
            self.assertIn(token, build)
        floors = json.loads((ROOT / "tools/host_coverage_floor.json").read_text(encoding="utf-8"))["modules"]
        for module in ("Common/boot_nor_stage.h", "Common/nor_spi5_pins.h"):
            self.assertEqual(floors["src/mcu_stm32h757/" + module], 100)

    def test_the_cfg_of_the_copy_and_the_pins_both_say_they_ship_off(self):
        self.assertIn("HYDRA_NOR_BACKUP 0", (ROOT / "config/nor_backup.cfg").read_text(encoding="utf-8"))
        self.assertIn("HYDRA_NOR_BACKUP 0", (ROOT / "config/nor_spi5.cfg").read_text(encoding="utf-8"))
        self.assertEqual(nb.load_config()["slot_bytes"], 786432)


if __name__ == "__main__":
    unittest.main()
