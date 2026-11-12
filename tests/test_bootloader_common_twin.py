# =============================================================================
# HYDRA-UMC - tests/test_bootloader_common_twin.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Host guard for the three bootloader headers: what they must share stays equal, what must differ does.

The G474 board bootloader and the two cores of the kinematic brain each carry their own bootloader_common.h. Until the
shared part moves into one common header, this test pins it: the command offsets, the status codes, the verify-fail
reasons and the metadata states are the same in the three, the metadata fields come in the same order (older fields
keep their offsets), and the things that identify a platform (hardware id, metadata magic, flash layout) differ and
are consistent. Pure text checks: nothing is compiled.
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADERS = {
    "g474": ROOT / "src" / "mcu_stm32g474" / "boot" / "bootloader_common.h",
    "cm4": ROOT / "src" / "mcu_stm32h745" / "CM4" / "boot" / "bootloader_common.h",
    "cm7": ROOT / "src" / "mcu_stm32h745" / "CM7" / "boot" / "bootloader_common.h",
}
SHARED_PREFIXES = ("OFS_", "STATUS_", "VERIFY_FAIL_REASON_", "META_STATE_")


def read(name):
    return HEADERS[name].read_text(encoding="utf-8", errors="replace")


def defines(text):
    found = {}
    for match in re.finditer(r"^#define\s+(\w+)\s+(.+?)\s*(?://.*)?$", text, re.M):
        found[match.group(1)] = match.group(2).strip()
    return found


def metadata_fields(text):
    body = re.search(r"typedef struct \{(.*?)\} FirmwareMetadata_t;", text, re.S).group(1)
    body = re.sub(r"//[^\n]*", "", body)
    return re.findall(r"(uint\d+_t)\s+(\w+(?:\[\d+\])?)\s*;", body)


def number(value):
    return int(re.sub(r"[uUlL]+$", "", value), 0)


class SharedProtocolTests(unittest.TestCase):
    def setUp(self):
        self.defs = {name: defines(read(name)) for name in HEADERS}

    def shared_names(self):
        names = set.intersection(*[set(d) for d in self.defs.values()])
        return sorted(n for n in names if n.startswith(SHARED_PREFIXES))

    def test_the_headers_exist(self):
        for path in HEADERS.values():
            self.assertTrue(path.is_file(), path)

    def test_there_is_a_real_shared_protocol(self):
        names = self.shared_names()
        self.assertGreater(len(names), 40)
        for prefix in SHARED_PREFIXES:
            self.assertTrue(any(n.startswith(prefix) for n in names), prefix)

    def test_the_shared_constants_have_one_value_in_the_three_headers(self):
        for name in self.shared_names():
            values = {board: self.defs[board][name] for board in HEADERS}
            self.assertEqual(len(set(values.values())), 1, f"{name}: {values}")

    # Known, deliberate differences: the axis offsets belong to the board with axes; the relay and poll offsets to the
    # two boards on the shared bus (the CM7 core only sees what the CM4 relays). Anything else missing somewhere is drift.
    KNOWN_PARTIAL = {
        "OFS_AXIS_STATUS": ["g474"],
        "OFS_AXIS_STEP_TELEMETRY": ["g474"],
        "OFS_AXIS_STATUS_RESPONSE": ["g474", "cm4"],
        "OFS_POLL_STATUS": ["g474", "cm4"],
        "OFS_RELAY_RECV": ["g474", "cm4"],
        "OFS_RELAY_RECV_RESPONSE": ["g474", "cm4"],
        "OFS_RELAY_SEND": ["g474", "cm4"],
    }

    def test_a_constant_of_the_shared_families_is_in_all_headers_unless_known(self):
        union = set()
        for d in self.defs.values():
            union |= {n for n in d if n.startswith(SHARED_PREFIXES)}
        only_some = {n: [b for b in HEADERS if n in self.defs[b]] for n in union - set(self.shared_names())}
        self.assertEqual(only_some, self.KNOWN_PARTIAL)

    def test_command_offsets_are_unique_inside_each_header(self):
        for board, d in self.defs.items():
            offsets = {}
            for name, value in d.items():
                if name.startswith("OFS_") and re.fullmatch(r"(0x[0-9A-Fa-f]+|\d+)[uU]?", value):
                    offsets.setdefault(number(value), []).append(name)
            for offset, names in offsets.items():
                self.assertEqual(len(names), 1, f"{board}: 0x{offset:X} used by {names}")

    def test_every_offset_fits_the_slot_window(self):
        for board, d in self.defs.items():
            for name, value in d.items():
                if name.startswith("OFS_") and re.fullmatch(r"(0x[0-9A-Fa-f]+|\d+)[uU]?", value):
                    self.assertLess(number(value), 64, f"{board} {name}")

    def test_status_and_reason_codes_are_unique_inside_each_header(self):
        for board, d in self.defs.items():
            for prefix in ("STATUS_", "VERIFY_FAIL_REASON_"):
                codes = [number(v) for n, v in d.items() if n.startswith(prefix) and re.fullmatch(r"(0x[0-9A-Fa-f]+|\d+)[uU]?", v)]
                self.assertEqual(len(codes), len(set(codes)), f"{board} {prefix}")


class MetadataLayoutTests(unittest.TestCase):
    def test_the_metadata_fields_come_in_the_same_order(self):
        layouts = {name: metadata_fields(read(name)) for name in HEADERS}
        self.assertEqual(layouts["g474"], layouts["cm4"])
        self.assertEqual(layouts["g474"], layouts["cm7"])

    def test_the_older_fields_keep_their_offsets(self):
        names = [n for _, n in metadata_fields(read("g474"))]
        self.assertEqual(names[:7], ["magic", "state", "hardware_id", "version_major", "version_minor", "size", "crc32"])
        self.assertEqual(names[-3:], ["boot_attempts", "version_patch", "counter"])

    def test_a_record_is_a_whole_number_of_flash_words(self):
        for name in HEADERS:
            d = defines(read(name))
            size = number(d["META_RECORD_SIZE"])
            self.assertEqual(size % 8, 0, name)
            self.assertGreaterEqual(size, 4 + 7 * 4 + 32 + 3 * 4 + 4, name)  # generation, 7 words, digest, 3 appended words, crc

    def test_the_record_stays_in_the_flash_word_of_its_platform(self):
        d = {name: defines(read(name)) for name in HEADERS}
        self.assertEqual(number(d["g474"]["META_RECORD_SIZE"]) % 8, 0)
        for core in ("cm4", "cm7"):
            self.assertEqual(number(d[core]["META_RECORD_SIZE"]) % 32, 0, core)  # H7 programs 32-byte words


class PlatformIdentityTests(unittest.TestCase):
    def setUp(self):
        self.defs = {name: defines(read(name)) for name in HEADERS}

    def test_hardware_ids_and_metadata_magics_all_differ(self):
        ids = [number(self.defs[b]["THIS_HARDWARE_ID"]) for b in HEADERS]
        magics = [number(self.defs[b]["METADATA_MAGIC_VALID"]) for b in HEADERS]
        self.assertEqual(len(set(ids)), 3)
        self.assertEqual(len(set(magics)), 3)

    def test_hardware_ids_spell_their_platform(self):
        def text(value):
            return number(value).to_bytes(4, "big").decode("ascii")

        self.assertEqual(text(self.defs["g474"]["THIS_HARDWARE_ID"]), "G401")
        self.assertEqual(text(self.defs["cm4"]["THIS_HARDWARE_ID"]), "H7C4")
        self.assertEqual(text(self.defs["cm7"]["THIS_HARDWARE_ID"]), "H7C7")

    def test_the_flash_layout_of_each_platform_is_ordered_and_does_not_overlap(self):
        for board, d in self.defs.items():
            metadata = number(d["METADATA_ADDR"])
            main = number(d["MAIN_APP_ADDR"])
            backup = number(d["BACKUP_APP_ADDR"])
            size = number(d["APP_MAX_SIZE"].strip("()").replace("UL", "").replace(" ", "").split("*")[0]) * 1024
            self.assertLess(metadata, main, board)
            main_range = (main, main + size)
            backup_range = (backup, backup + size)
            self.assertTrue(main_range[1] <= backup_range[0] or backup_range[1] <= main_range[0], f"{board}: application and backup overlap")

    def test_the_version_numbers_are_small_integers(self):
        for board, d in self.defs.items():
            for name in ("BOOTLOADER_VERSION_MAJOR", "BOOTLOADER_VERSION_MINOR", "BOOTLOADER_VERSION_PATCH", "FIRMWARE_VERSION_MAJOR", "FIRMWARE_VERSION_MINOR", "FIRMWARE_VERSION_PATCH"):
                self.assertTrue(0 <= number(d[name]) <= 255, f"{board} {name}")


if __name__ == "__main__":
    unittest.main()
