# =============================================================================
# HYDRA-UMC - tests/test_renode_g474_platform.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Guards over the hand-written Renode platform of the STM32G474: the memory map is the one of the flash geometry of the bootloader, the workflow
pins the same Renode release as the platform check, and the documentation names what is not modelled. Evidence: simulator (source check)."""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPL = (ROOT / "tools" / "renode" / "stm32g474.repl").read_text(encoding="utf-8")
GEOMETRY = (ROOT / "src" / "mcu_stm32g474" / "boot" / "flash_layer.h").read_text(encoding="utf-8")


def block(name: str) -> dict:
    found = re.search(rf"^{name}:.*?@ sysbus (<)?(0x[0-9A-Fa-f]+)(?:, \+(0x[0-9A-Fa-f]+)>)?\s*\n((?:    .*\n)*)", REPL, re.M)
    assert found, name
    size = re.search(r"size: (0x[0-9A-Fa-f]+)", found.group(4))
    return {"base": int(found.group(2), 16), "size": int(size.group(1), 16) if size else None}


class PlatformMemoryMap(unittest.TestCase):
    def test_the_flash_is_the_one_the_bootloader_geometry_declares(self):
        geometry = re.search(r"FlashLayer_GeometryG474.*?\{(0x[0-9A-Fa-f]+)u, (0x[0-9A-Fa-f]+)u", GEOMETRY, re.S)
        self.assertIsNotNone(geometry)
        flash = block("flash")
        self.assertEqual(flash["base"], int(geometry.group(1), 16))
        self.assertEqual(flash["size"], int(geometry.group(2), 16))

    def test_the_ram_blocks_do_not_overlap_and_the_ccm_alias_is_as_large_as_the_ccm(self):
        spans = []
        for name in ("sram", "ccm_sram", "ccm_sram_alias", "fdcan_message_ram", "flash"):
            item = block(name)
            spans.append((item["base"], item["base"] + item["size"], name))
        spans.sort()
        for (_, end, first), (start, _, second) in zip(spans, spans[1:]):
            self.assertLessEqual(end, start, f"{first} overlaps {second}")
        self.assertEqual(block("ccm_sram")["size"], block("ccm_sram_alias")["size"])

    def test_the_peripherals_sit_at_the_addresses_of_the_reference_manual(self):
        expected = {"usart1": 0x40013800, "spi1": 0x40013000, "spi2": 0x40003800, "iwdg": 0x40003000, "gpioPortA": 0x48000000, "gpioPortG": 0x48001800}
        for name, address in expected.items():
            self.assertEqual(block(name)["base"], address, name)

    def test_the_cpu_is_a_cortex_m4_with_an_fpu(self):
        self.assertIn('cpuType: "cortex-m4f"', REPL)


class WorkflowAndDocs(unittest.TestCase):
    def test_both_renode_workflows_pin_the_same_release_and_hash(self):
        pins = []
        for name in ("renode-check.yml", "renode-g474.yml"):
            text = (ROOT / ".github" / "workflows" / name).read_text(encoding="utf-8")
            pins.append((re.search(r'RENODE_VERSION: "([^"]+)"', text).group(1), re.search(r'RENODE_SHA256: "([0-9a-f]{64})"', text).group(1)))
        self.assertEqual(pins[0], pins[1])

    def test_the_workflow_is_started_by_hand_only(self):
        text = (ROOT / ".github" / "workflows" / "renode-g474.yml").read_text(encoding="utf-8")
        self.assertIn("workflow_dispatch:", text)
        self.assertNotIn("push:", text)
        self.assertNotIn("pull_request", text)

    def test_the_documentation_says_what_is_not_modelled(self):
        text = (ROOT / "docs" / "RENODE.md").read_text(encoding="utf-8")
        for word in ("RCC", "flash controller", "FDCAN protocol engine"):
            self.assertIn(word, text)
        self.assertIn("docs/RENODE.md", REPL)


if __name__ == "__main__":
    unittest.main()
