# =============================================================================
# HYDRA-UMC - tests/test_ipc_link.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The operations on the mailbox between the two cores (ipc_link.h) name the gateway as the owner of SPI1, FDCAN1 and I2C4 in the platform profile, are free of the HAL, keep the
rules of the frames (the frame is not written while the other core may read it, the barrier comes before the semaphore is taken), and the host test is registered in both runners
for both platforms and both settings of the seal switch. Evidence: simulator (text checks); the host test runs the operations."""
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import platform_profile as pp  # noqa: E402

HEADER = (ROOT / "src/mcu_stm32h757/Common/ipc_link.h").read_text(encoding="utf-8")


class IpcLinkTests(unittest.TestCase):
    def test_the_owner_of_the_host_link_is_the_gateway_on_each_platform(self):
        for name, gateway in (("h745_current", "CM4"), ("h757_target", "CM7")):
            owner = pp.profile(name)["peripheral_owner"]
            self.assertEqual({owner["SPI1"], owner["FDCAN1"], owner["I2C4"]}, {gateway}, name)
        self.assertIn("#define IPC_GATEWAY_CORE HYDRA_OWNER_SPI1", HEADER)

    def test_the_header_is_free_of_the_hal(self):
        self.assertNotIn("HAL_", HEADER)
        self.assertNotIn("__DSB", HEADER)   # the barrier is a callback
        self.assertNotIn("0x38000000", HEADER)   # the mailbox is given by the caller

    def test_a_busy_channel_leaves_the_frame_alone_and_the_barrier_comes_before_the_take(self):
        post = HEADER.split("static inline IpcStatus Ipc_Post(", 1)[1].split("\nstatic inline", 1)[0]
        self.assertLess(post.index("return IPC_BUSY"), post.index("Ipc_Fill"))
        self.assertLess(post.index("Ipc_Fill"), post.index("ops->barrier"))
        self.assertLess(post.index("ops->barrier"), post.index("ops->sem_take(ctx, posted)"))

    def test_the_consumer_copies_checks_and_then_takes_its_own_semaphore_and_never_releases_the_posters(self):
        take = HEADER.split("static inline IpcStatus Ipc_Take(", 1)[1].split("\nstatic inline", 1)[0]
        self.assertLess(take.index("Ipc_Copy"), take.index("IpcFrame_Valid"))
        self.assertLess(take.index("IpcFrame_Valid"), take.index("ops->sem_take(ctx, done)"))
        self.assertNotIn("sem_release", take)

    def test_every_release_in_the_header_is_of_a_semaphore_the_same_core_took(self):
        post_finish = HEADER.split("static inline void Ipc_FinishPost(", 1)[1].split("\n}", 1)[0]
        consume_finish = HEADER.split("static inline void Ipc_FinishConsume(", 1)[1].split("\n}", 1)[0]
        self.assertIn("ops->sem_release(ctx, posted)", post_finish)    # the poster releases POSTED, which it took
        self.assertIn("ops->sem_release(ctx, done)", consume_finish)   # the consumer releases DONE, which it took
        self.assertEqual(HEADER.count("sem_release(ctx"), 2)

    def test_the_four_mailbox_semaphores_and_the_clock_tree_one_are_five_different_numbers(self):
        mailbox = (ROOT / "src/mcu_stm32h757/Common/ipc_mailbox.h").read_text(encoding="utf-8")
        owner = (ROOT / "src/mcu_stm32h757/CM4/rcc_owner.h").read_text(encoding="utf-8")
        ids = [int(m) for m in re.findall(r"#define IPC_HSEM_(?:CMD|RESP)(?:_DONE)?_ID\s+(\d+)", mailbox)]
        rcc = int(re.search(r"#define HYDRA_HSEM_RCC (\d+)u", owner).group(1))
        self.assertEqual(len(ids), 4)
        self.assertEqual(len(set(ids + [rcc])), 5)

    def test_no_source_releases_or_takes_a_semaphore_outside_the_hal_layer_and_the_clock_tree_handshake(self):
        allowed = {"src/mcu_stm32h757/Common/ipc_hal.h", "src/mcu_stm32h757/CM4/STM32H757XI_CM4_main.c"}
        for path in (ROOT / "src").rglob("*.[ch]"):
            relative = path.relative_to(ROOT).as_posix()
            text = path.read_text(encoding="utf-8", errors="ignore")
            if relative not in allowed:
                self.assertNotIn("HAL_HSEM_Release", text, relative)
                self.assertNotIn("HAL_HSEM_FastTake", text, relative)

    def test_the_document_describes_the_four_semaphores_and_who_releases_what(self):
        doc = (ROOT / "docs/CANBUS_STM32H745.md").read_text(encoding="utf-8")
        for needle in ("IPC_HSEM_CMD_DONE_ID", "IPC_HSEM_RESP_DONE_ID", "ONLY by the core that took it", "releases only the"):
            self.assertIn(needle, doc)
        self.assertNotIn("releases it (frees the", doc)   # the sentence of the old protocol

    def test_the_times_are_the_ones_the_bootloaders_use_today(self):
        for needle in ("IPC_POST_COMMAND_WAIT_MS  50u", "IPC_WAIT_RESPONSE_MS      200u", "IPC_POST_RESPONSE_WAIT_MS 100u"):
            self.assertIn(needle, HEADER)

    def test_the_host_test_is_registered_in_both_runners_for_both_platforms_and_seal_settings(self):
        host = (ROOT / "tools/host_tests.py").read_text(encoding="utf-8")
        script = (ROOT / "build_firmware.sh").read_text(encoding="utf-8")
        self.assertIn("tests/test_ipc_link.c", host)
        self.assertIn("tests/test_ipc_link.c", script)
        self.assertEqual(host.count('"tests/test_ipc_link.c"'), 4)   # H745 and H757, seal switch off and on


if __name__ == "__main__":
    unittest.main()
