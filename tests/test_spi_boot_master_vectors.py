# =============================================================================
# HYDRA-UMC - tests/test_spi_boot_master_vectors.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The vectors of the C header spi_boot_master.h are what the Python model produces, the header is registered in the host tests and its constants are the model's.
Evidence: simulator (the C test itself is compiled and run in the CI by tools/host_tests.py)."""
import json
import re
import sys
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import gen_spi_boot_vectors as gen  # noqa: E402
import host_tests  # noqa: E402
import spi_boot_master as sbm  # noqa: E402

HEADER = (ROOT / "src" / "mcu_stm32h757" / "Common" / "spi_boot_master.h").read_text(encoding="utf-8")


class VectorTests(unittest.TestCase):
    def test_the_vector_files_are_what_the_model_produces(self):
        self.assertEqual(gen.main(["--check"]), 0)

    def test_the_vectors_cover_every_frame_type_every_refusal_and_every_verdict(self):
        vectors = json.loads(gen.JSON_PATH.read_text(encoding="utf-8"))
        self.assertEqual({f["type"] for f in vectors["frames"]}, {sbm.T_HELLO, sbm.T_GET_PAGE, sbm.T_GET_TOKEN, sbm.T_DONE, sbm.T_OFFER, sbm.T_PAGE, sbm.T_TOKEN, sbm.T_REFUSE})
        self.assertEqual({b["status"] for b in vectors["bad_frames"]}, {1, 2, 3, 4, 5})
        self.assertEqual({o["verdict"] for o in vectors["offers"]}, {sbm.OFFER_OK, sbm.OFFER_BAD_SIZE, sbm.OFFER_WRONG_TARGET, sbm.OFFER_BOOTLOADER_TOO_OLD})

    def test_every_good_frame_decodes_and_every_bad_one_is_refused_by_the_model(self):
        vectors = json.loads(gen.JSON_PATH.read_text(encoding="utf-8"))
        for f in vectors["frames"]:
            kind, sequence, payload = sbm.decode(bytes.fromhex(f["frame_hex"]), sbm.HOST_TYPES if f["host"] else sbm.MASTER_TYPES)
            self.assertEqual((kind, sequence, payload.hex()), (f["type"], f["sequence"], f["payload_hex"]), f["name"])
        for b in vectors["bad_frames"]:
            with self.assertRaises(sbm.FrameError, msg=b["name"]):
                sbm.decode(bytes.fromhex(b["frame_hex"]), sbm.HOST_TYPES if b["host"] else sbm.MASTER_TYPES)

    def test_the_crc_of_the_check_string_is_the_standard_value(self):
        self.assertEqual(zlib.crc32(b"123456789"), 0xCBF43926)
        self.assertIn("0xCBF43926", (ROOT / "tests" / "test_spi_boot_master.c").read_text(encoding="utf-8"))

    def test_the_constants_of_the_header_are_the_ones_of_the_model(self):
        def define(name):
            return int(re.search(rf"#define {name}\s+(?:\(?)(0x[0-9A-Fa-f]+|\d+)u", HEADER).group(1), 0)

        self.assertEqual(define("SBM_SYNC"), sbm.SYNC)
        self.assertEqual(define("SBM_PAGE_BYTES"), sbm.MAX_PAGE)
        self.assertEqual(define("SBM_HEADER_BYTES") + 4, sbm.FRAME_OVERHEAD)
        for name, value in (("HELLO", sbm.T_HELLO), ("GET_PAGE", sbm.T_GET_PAGE), ("GET_TOKEN", sbm.T_GET_TOKEN), ("DONE", sbm.T_DONE), ("OFFER", sbm.T_OFFER), ("PAGE", sbm.T_PAGE), ("TOKEN", sbm.T_TOKEN), ("REFUSE", sbm.T_REFUSE)):
            self.assertEqual(define(f"SBM_T_{name}"), value, name)
        self.assertEqual(define("SBM_OFFER_BYTES"), 32)
        self.assertEqual(define("SBM_IMAGE_OVERHEAD"), 128)

    def test_the_header_is_header_only_and_uses_no_hal(self):
        self.assertNotIn("stm32h7xx", HEADER)
        self.assertNotIn("HAL_", HEADER)

    def test_the_host_test_is_in_the_table_of_the_runner_and_in_the_build_script(self):
        names = [test.name for test in host_tests.table()]
        self.assertIn("spi_boot_master", names)
        script = (ROOT / "build_firmware.sh").read_text(encoding="utf-8")
        self.assertIn("tests/test_spi_boot_master.c", script)
        self.assertEqual(sum(1 for t in host_tests.table() if t.source == "tests/test_spi_boot_master.c"), 1)

    def test_the_judgement_of_the_model_follows_the_rules_of_the_document(self):
        cfg = sbm.BootConfig(spi_master_enabled=True)
        self.assertEqual(sbm.judge_offer(1428, "", None, cfg), sbm.OFFER_OK)
        self.assertEqual(sbm.judge_offer(10, "", None, cfg), sbm.OFFER_BAD_SIZE)
        self.assertEqual(sbm.judge_offer(1428, "OTHER", None, cfg), sbm.OFFER_WRONG_TARGET)
        self.assertEqual(sbm.judge_offer(1428, "", (9, 0, 0), cfg), sbm.OFFER_BOOTLOADER_TOO_OLD)


if __name__ == "__main__":
    unittest.main()
