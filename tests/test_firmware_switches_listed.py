"""Every switch that a CM7 driver header ships off is named in docs/FIRMWARE_SWITCHES.md."""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
H757 = ROOT / "src" / "mcu_stm32h757"


class FirmwareSwitchesListed(unittest.TestCase):
    def test_every_off_switch_of_the_cm7_drivers_is_in_the_document(self):
        doc = (ROOT / "docs" / "FIRMWARE_SWITCHES.md").read_text(encoding="utf-8")
        headers = sorted((H757 / "CM7").glob("*.h")) + [H757 / "Common" / "ipc_mailbox.h"]
        found = set()
        for header in headers:
            text = header.read_text(encoding="utf-8", errors="replace")
            found.update(re.findall(r"^#ifndef (HYDRA_[A-Z0-9_]+)\s*\n#define \1 0\b", text, re.M))
        self.assertGreaterEqual(len(found), 13)
        missing = sorted(name for name in found if f"`{name}`" not in doc)
        self.assertEqual(missing, [], "switches that ship off and are not in docs/FIRMWARE_SWITCHES.md")


if __name__ == "__main__":
    unittest.main()
