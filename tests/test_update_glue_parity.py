# =============================================================================
# HYDRA-UMC - tests/test_update_glue_parity.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The three bootloaders (G474 over CAN, H745 CM4 over SPI, H745 CM7 over the mailbox) each carry the glue between the frames and the
decision of update_verify.h. The decision itself is one file with host tests; the glue is checked end to end on the G474
(tests/test_bootloader_g474_update_host.c) and only syntax-checked for the two H745 cores, so this test keeps the three copies identical
except for the few lines that are the core's own (how a frame is sent, its RAM range, what happens after success)."""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CORES = {
    "g474": ROOT / "src/mcu_stm32g474/boot/bootloader_protocol.c",
    "cm4": ROOT / "src/mcu_stm32h745/CM4/boot/bootloader_protocol.c",
    "cm7": ROOT / "src/mcu_stm32h745/CM7/boot/bootloader_protocol.c",
}
SAME_FUNCTIONS = (
    "HandleImageHeaderChunk",
    "HandleDowngradeNonceRequest",
    "HandleDowngradeTokenChunk",
    "MakeSessionNonce",
    "Boot_SessionEntropy",
    "UpdateSignatureUnderTheKey",
    "HandleHmacChunk",
    "VerifyFailReasonOf",
    "HandleAuthorizeDowngrade",
)


def read(path: Path) -> str:
    return path.read_bytes().decode("utf-8").replace("\r\n", "\n")


def function_body(text: str, name: str) -> str:
    """The text of the function `name` from its signature to its closing brace at column 0."""
    match = re.search(rf"^(?:static |__attribute__\(\(weak\)\) )?[A-Za-z_][\w \*]*\b{re.escape(name)}\(", text, re.M)
    if not match:
        raise AssertionError(f"{name} not found")
    end = text.index("\n}\n", match.start()) + 3
    return text[match.start():end]


def end_update_common_part(text: str) -> str:
    body = function_body(text, "HandleEndUpdate")
    body = body[:body.index("CAN_SendStatus(STATUS_VERIFY_OK);")]
    body = re.sub(r"input\.expect\.ram_lo = [^;]+;[^\n]*", "input.expect.ram_lo = RANGE;", body)
    body = re.sub(r"input\.expect\.ram_hi = [^;]+;[^\n]*", "input.expect.ram_hi = RANGE;", body)
    return body


class GlueParityTests(unittest.TestCase):
    def test_the_shared_functions_are_the_same_in_the_three_bootloaders(self):
        texts = {name: read(path) for name, path in CORES.items()}
        for function in SAME_FUNCTIONS:
            reference = function_body(texts["g474"], function)
            for core in ("cm4", "cm7"):
                self.assertEqual(function_body(texts[core], function), reference, f"{function} differs in {core}")

    def test_the_end_of_an_update_is_the_same_up_to_the_success_status(self):
        texts = {name: read(path) for name, path in CORES.items()}
        reference = end_update_common_part(texts["g474"])
        for core in ("cm4", "cm7"):
            self.assertEqual(end_update_common_part(texts[core]), reference, f"HandleEndUpdate differs in {core}")

    def test_each_core_sends_the_nonce_with_its_own_sender_and_keeps_its_own_ram_range(self):
        texts = {name: read(path) for name, path in CORES.items()}
        self.assertIn("FDCAN_Send(g_slot_base + OFS_DOWNGRADE_NONCE_RESPONSE, half, 8);", function_body(texts["g474"], "SendNonceHalf"))
        self.assertIn("StashResponse(OFS_DOWNGRADE_NONCE_RESPONSE, half, 8);", function_body(texts["cm4"], "SendNonceHalf"))
        self.assertIn("IPC_SendResponse(OFS_DOWNGRADE_NONCE_RESPONSE, half, 8);", function_body(texts["cm7"], "SendNonceHalf"))
        self.assertIn("SRAM_BASE + 0x100UL", function_body(texts["g474"], "HandleEndUpdate"))
        self.assertIn("D2_AHBSRAM_BASE + 0x400UL", function_body(texts["cm4"], "HandleEndUpdate"))
        self.assertIn("D1_AXISRAM_BASE + 0x400UL", function_body(texts["cm7"], "HandleEndUpdate"))

    def test_the_end_of_an_update_uses_the_same_ram_range_as_the_jump(self):
        pairs = {
            "g474": ("SRAM_BASE + 0x100UL", "SRAM_BASE + 0x18000UL"),
            "cm4": ("D2_AHBSRAM_BASE + 0x400UL", "D2_AHBSRAM_BASE + 0x48000UL"),
            "cm7": ("D1_AXISRAM_BASE + 0x400UL", "D1_AXISRAM_BASE + 0x80000UL"),
        }
        for core, (low, high) in pairs.items():
            text = read(CORES[core])
            check = function_body(text, "StackPointerInValidRAM")
            self.assertIn(low, check, core)
            self.assertIn(high, check, core)
            update = function_body(text, "HandleEndUpdate")
            self.assertIn(f"input.expect.ram_lo = {low};", update, core)
            self.assertIn(f"input.expect.ram_hi = {high};", update, core)

    def test_every_core_dispatches_the_new_frames(self):
        mains = {
            "g474": ROOT / "src/mcu_stm32g474/boot/bootloader_main.c",
            "cm4": ROOT / "src/mcu_stm32h745/CM4/boot/bootloader_main.c",
            "cm7": ROOT / "src/mcu_stm32h745/CM7/boot/bootloader_main.c",
        }
        for core, path in mains.items():
            text = read(path)
            for handler in ("HandleImageHeaderChunk", "HandleDowngradeTokenChunk", "HandleDowngradeNonceRequest"):
                self.assertIn(handler, text, f"{core} does not dispatch {handler}")

    def test_the_frame_numbers_are_the_same_in_the_three_common_headers(self):
        commons = {
            "g474": ROOT / "src/mcu_stm32g474/boot/bootloader_common.h",
            "cm4": ROOT / "src/mcu_stm32h745/CM4/boot/bootloader_common.h",
            "cm7": ROOT / "src/mcu_stm32h745/CM7/boot/bootloader_common.h",
        }
        expected = {"OFS_IMAGE_HEADER": 0x19, "OFS_DOWNGRADE_NONCE_REQUEST": 0x1A, "OFS_DOWNGRADE_TOKEN": 0x1B, "OFS_DOWNGRADE_NONCE_RESPONSE": 0x1C, "VERIFY_FAIL_REASON_HEADER": 0x06}
        for core, path in commons.items():
            text = read(path)
            for name, value in expected.items():
                match = re.search(rf"^#define {name}\s+(0x[0-9A-Fa-f]+)", text, re.M)
                self.assertIsNotNone(match, f"{core}: {name}")
                self.assertEqual(int(match.group(1), 16), value, f"{core}: {name}")
            self.assertIn("uint32_t version_patch;", text, core)
            self.assertIn("#define METADATA_PATCH(meta)", text, core)


if __name__ == "__main__":
    unittest.main()
