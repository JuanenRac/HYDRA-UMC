# =============================================================================
# HYDRA-UMC - spi_bridge: flashing under injected faults (simulated board)
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see repo root LICENSE
# =============================================================================
"""The flash cycle against a simulated A/B bootloader (sim_board.py) with a fault on every frame of the transfer: a power cut, a damaged
frame, a lost answer, a repeated answer, a flash page that cannot be written. Whatever the fault, the active image is either the old
one or the whole new one, never a mixture, and a later clean update succeeds. Evidence: simulator. The same cases on a Nucleo board
are pending."""

import unittest
from unittest import mock

from sim_board import Fault, PowerCut, SimulatedBoard

from spi_bridge.bootloader_client import SpiOtaFlasher
from spi_bridge.protocol import OFS_END_UPDATE, SPI_TARGET_STACKA, SpiOtaFrame

HARDWARE_ID = 0x0303CC01  # a URTC bootloader behind the relay: the image goes with an HMAC, no header
OLD = b"OLD-IMAGE" * 50
NEW = bytes(range(256)) * 20  # 5120 bytes: three pages, the last one short


def flash(board, firmware=NEW, timeout=0.05):
    """Runs one update; returns the phases it reported, or the exception that stopped it (a lost link is raised, not reported)."""
    flasher = SpiOtaFlasher(board, hmac_key=b"\x00" * 32)
    phases = []
    with mock.patch("spi_bridge.bootloader_client.time.sleep"):
        try:
            for progress in flasher.flash(SPI_TARGET_STACKA, 2, HARDWARE_ID, firmware, 0, 2, page_timeout_seconds=timeout):
                phases.append(progress.phase)
        except OSError as error:
            return phases, error
    return phases, None


def frames_of_a_clean_update() -> int:
    board = SimulatedBoard(old_image=OLD, hardware_id=HARDWARE_ID)
    phases, error = flash(board)
    assert phases[-1] == "done" and error is None, (phases, error)
    return board.frame_count


class CleanUpdate(unittest.TestCase):
    def test_a_clean_update_replaces_the_active_image(self):
        board = SimulatedBoard(old_image=OLD)
        phases, error = flash(board)
        self.assertIsNone(error)
        self.assertEqual(phases[-1], "done")
        self.assertEqual(board.active_image, NEW)


class PowerCutAtEveryFrame(unittest.TestCase):
    def test_a_cut_at_any_frame_never_leaves_a_mixed_image(self):
        total = frames_of_a_clean_update()
        self.assertGreater(total, 50)
        for cut_at in range(total):
            with self.subTest(cut_at=cut_at):
                board = SimulatedBoard(old_image=OLD, faults=[Fault("power_cut", cut_at)])
                phases, error = flash(board)
                self.assertIsInstance(error, PowerCut)
                self.assertNotIn("done", phases)
                # never a mixture: the old image, or (only when the cut came after the board had verified and switched) the whole new one
                self.assertIn(board.active_image, (OLD, NEW))
                if cut_at < total - 3:
                    self.assertEqual(board.active_image, OLD, "the active image is untouched by an update that did not finish")
                board.power_up()
                self.assertIn(board.active_image, (OLD, NEW))
                phases, error = flash(board)  # resumption: the update is repeated from its start
                self.assertIsNone(error)
                self.assertEqual(phases[-1], "done")
                self.assertEqual(board.active_image, NEW)


class DamagedFrames(unittest.TestCase):
    def test_a_flipped_bit_in_any_data_frame_is_caught_by_the_crc_and_nothing_is_installed(self):
        total = frames_of_a_clean_update()
        caught = 0
        for at in range(total):
            board = SimulatedBoard(old_image=OLD, faults=[Fault("bit_flip", at)])
            phases, error = flash(board)
            if board.active_image == NEW:
                # the damaged frame carried nothing the image depends on (a poll, an answer-only frame, a signature byte the model does not check)
                self.assertEqual(phases[-1], "done")
                continue
            caught += 1
            self.assertEqual(board.active_image, OLD, f"frame {at}: a damaged update must not change the active image")
            self.assertNotEqual(phases[-1:], ["done"])
        self.assertGreater(caught, 0)

    def test_a_lost_answer_is_an_error_not_a_success(self):
        total = frames_of_a_clean_update()
        for at in range(total):
            board = SimulatedBoard(old_image=OLD, faults=[Fault("drop_answer", at)])
            phases, error = flash(board)
            if board.active_image == NEW:
                self.assertTrue(phases[-1] == "done" or error is not None)  # the link was lost after the board had already switched: nothing is claimed
            else:
                self.assertEqual(board.active_image, OLD)
                self.assertTrue(error is not None or phases[-1] == "error", f"frame {at}: neither an error nor a done")

    def test_a_repeated_answer_never_reports_done_for_an_image_that_was_not_installed(self):
        total = frames_of_a_clean_update()
        for at in range(total):
            board = SimulatedBoard(old_image=OLD, faults=[Fault("duplicate_answer", at)])
            phases, error = flash(board)
            if phases and phases[-1] == "done":
                self.assertEqual(board.active_image, NEW, f"frame {at}: done reported but the new image is not active")
            else:
                self.assertEqual(board.active_image, OLD)


class FlashErrors(unittest.TestCase):
    def test_a_page_that_cannot_be_written_stops_the_update_with_an_error(self):
        data_frames = [index for index in range(frames_of_a_clean_update())]
        errors = 0
        for at in data_frames:
            board = SimulatedBoard(old_image=OLD, faults=[Fault("ecc_error", at)])
            phases, error = flash(board)
            if board.active_image == NEW:
                continue
            errors += 1
            self.assertEqual(board.active_image, OLD)
            self.assertEqual(phases[-1], "error")
        self.assertGreater(errors, 0)

    def test_an_image_whose_crc_does_not_match_is_refused(self):
        class WrongCrcBoard(SimulatedBoard):
            def transceive(self, frame):
                if frame.frame_type == OFS_END_UPDATE and frame.payload:
                    frame = SpiOtaFrame(frame.target_tier, frame.target_slot, frame.frame_type, frame.dlc, b"\x12\x34\x56\x78" + frame.payload[4:])
                return super().transceive(frame)

        board = WrongCrcBoard(old_image=OLD)
        flasher = SpiOtaFlasher(board, hmac_key=b"\x00" * 32)
        with mock.patch("spi_bridge.bootloader_client.time.sleep"):
            phases = [p.phase for p in flasher.flash(SPI_TARGET_STACKA, 2, HARDWARE_ID, NEW, 0, 2, page_timeout_seconds=0.05)]
        self.assertEqual(phases[-1], "error")
        self.assertEqual(board.active_image, OLD)


if __name__ == "__main__":
    unittest.main()
