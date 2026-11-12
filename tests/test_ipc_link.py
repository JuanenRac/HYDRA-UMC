# =============================================================================
# HYDRA-UMC - tests/test_ipc_link.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The operations on the mailbox between the two cores (ipc_link.h) name the gateway as the owner of SPI1, FDCAN1 and I2C4 in the platform profile, are free of the HAL, keep the
rules of the frames (the frame is not written while the other core may read it, the barrier comes before the semaphore is taken), and the host test is registered in both runners
for both platforms and both settings of the seal switch. Evidence: simulator (text checks); the host test runs the operations."""
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
        self.assertLess(post.index("ops->barrier"), post.index("ops->sem_take(ctx"))

    def test_the_take_copies_checks_and_always_releases(self):
        take = HEADER.split("static inline IpcStatus Ipc_Take(", 1)[1].split("\nstatic inline", 1)[0]
        self.assertLess(take.index("Ipc_Copy"), take.index("IpcFrame_Valid"))
        self.assertLess(take.index("IpcFrame_Valid"), take.index("ops->sem_release"))

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
