# =============================================================================
# HYDRA-UMC - tests/test_spi_boot_master_fsm.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The state machine of the bootloader as SPI master (spi_boot_master_fsm.h) keeps the numbers and the golden session of the Python model, its switch is off by
default, and its host test is registered in both runners. The machine itself is run by tests/test_spi_boot_master_fsm.c. Evidence: simulator (text checks)."""
import json
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import spi_boot_master as sbm  # noqa: E402

FSM = (ROOT / "src/mcu_stm32h757/Common/spi_boot_master_fsm.h").read_text(encoding="utf-8")
VECTORS = json.loads((ROOT / "tests/spi_boot_master_vectors.json").read_text(encoding="utf-8"))


class SpiBootMasterFsmTests(unittest.TestCase):
    def test_the_switch_is_off_by_default_and_the_machine_checks_it_first(self):
        self.assertRegex(FSM, r"#ifndef HYDRA_BOOT_SPI_MASTER\n#define HYDRA_BOOT_SPI_MASTER 0\n#endif")
        body = FSM.split("static inline SbmResult Sbm_Receive", 1)[1]
        self.assertLess(body.index("if (!cfg->enabled)"), body.index("ops->stage_invalidate"))
        self.assertLess(body.index("if (!cfg->enabled)"), body.index("ops->nonce"))

    def test_the_retries_are_those_of_the_model(self):
        self.assertEqual(int(re.search(r"#define SBM_MAX_RETRIES (\d+)u", FSM).group(1)), sbm.MAX_RETRIES)

    def test_the_staging_slot_is_marked_valid_only_after_the_verification(self):
        body = FSM.split("static inline SbmResult Sbm_Receive", 1)[1]
        self.assertEqual(body.count("ops->stage_mark_valid"), 1)
        self.assertLess(body.index("ops->verify("), body.index("ops->stage_mark_valid"))
        self.assertIn("if (!ops->verify(", body)
        # every end that is not an acceptance invalidates the slot
        finish = FSM.split("static inline SbmResult Sbm_Finish", 1)[1].split("}", 1)[0]
        self.assertIn("result != SBM_RESULT_ACCEPTED", finish)
        self.assertIn("ops->stage_invalidate", finish)

    def test_the_machine_has_no_hal_and_touches_the_hardware_only_through_the_callbacks(self):
        self.assertNotIn("stm32h7xx", FSM)
        self.assertNotIn("HAL_", FSM)
        self.assertNotIn("SPI1->", FSM)

    def test_the_golden_session_is_the_requests_the_model_builds(self):
        session = VECTORS["session"]
        frames = session["request_frames_hex"]
        pages = -(-session["size"] // sbm.MAX_PAGE)
        self.assertEqual(len(frames), pages + 2)   # HELLO, one GET_PAGE per page, DONE
        kinds = [bytes.fromhex(f)[1] for f in frames]
        self.assertEqual(kinds, [sbm.T_HELLO] + [sbm.T_GET_PAGE] * pages + [sbm.T_DONE])
        sequences = [int.from_bytes(bytes.fromhex(f)[2:4], "little") for f in frames]
        self.assertEqual(sequences, list(range(1, len(frames) + 1)))
        for frame in frames:
            sbm.decode(bytes.fromhex(frame), sbm.MASTER_TYPES)   # a frame the model accepts

    def test_the_host_test_is_registered_in_both_runners(self):
        self.assertIn("tests/test_spi_boot_master_fsm.c", (ROOT / "tools/host_tests.py").read_text(encoding="utf-8"))
        self.assertIn("tests/test_spi_boot_master_fsm.c", (ROOT / "build_firmware.sh").read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
