"""Static guards over the firmware sources for mistakes that no host logic test can see."""
import re
import unittest
from pathlib import Path

SRC = Path(__file__).resolve().parent.parent / "src"
C_FILES = [p for p in SRC.rglob("*.c") if "cm5_host" not in p.parts]


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


class FirmwareSourcesTest(unittest.TestCase):
    def test_every_watchdog_sets_its_window(self):
        # IWDG_InitTypeDef.Window left at 0 makes the first refresh reset the chip.
        files = [p for p in C_FILES if "Init.Prescaler = IWDG" in p.read_text(encoding="utf-8", errors="replace")]
        self.assertGreaterEqual(len(files), 6)
        for path in files:
            code = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
            self.assertRegex(code, r"hiwdg\.Init\.Window\s*=\s*IWDG_WINDOW_DISABLE", str(path))

    def test_fdcan_data_length_is_never_shifted_by_hand(self):
        # The HAL puts DataLength into the message RAM word (and takes it back)
        # itself; the FDCAN_DLC_BYTES_n constants are the plain 0..8 code.
        for path in C_FILES:
            code = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
            self.assertNotRegex(code, r"DataLength\s*>>\s*16", str(path))
            self.assertNotRegex(code, r"DataLength\s*=[^;]*<<\s*16", str(path))


if __name__ == "__main__":
    unittest.main()
