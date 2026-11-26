"""generate_manifest.py: an image longer than its region is refused, the switch of the metadata in two sectors changes the room, and the report says what fits."""
import importlib.util
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
_spec = importlib.util.spec_from_file_location("hydra_generate_manifest", ROOT / "generate_manifest.py")
manifest = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(manifest)

KB = 1024


def _image(folder: Path, name: str, size: int) -> Path:
    """An image whose first words are a stack pointer in the CM7 RAM and a Thumb reset vector in its slot, padded to `size` bytes."""
    head = (0x24000100).to_bytes(4, "little") + (0x08040101).to_bytes(4, "little")
    path = folder / name
    path.write_bytes(head + bytes(size - len(head)))
    return path


class ImageCapTests(unittest.TestCase):
    def test_the_room_follows_the_switch_of_the_build(self):
        self.assertEqual(manifest.kb_app_region_bytes({}), 384 * KB)
        self.assertEqual(manifest.kb_app_region_bytes({"HYDRA_META_AB": "0"}), 384 * KB)
        self.assertEqual(manifest.kb_app_region_bytes({"HYDRA_META_AB": "1"}), 256 * KB)
        for environ in ({}, {"HYDRA_META_AB": ""}, {"HYDRA_META_AB": "yes"}):
            self.assertEqual(manifest.kb_app_region_bytes(environ), 256 * KB, environ)

    def test_an_image_longer_than_its_region_is_refused(self):
        with tempfile.TemporaryDirectory() as folder:
            at_limit = _image(Path(folder), "HYDRA_KB_CM7_APP_v0.0.1.bin", 256 * KB)
            manifest.check_image_fits(str(at_limit), 256 * KB)
            over = _image(Path(folder), "HYDRA_KB_CM7_APP_v0.0.2.bin", 256 * KB + 1)
            with self.assertRaises(SystemExit) as caught:
                manifest.check_image_fits(str(over), 256 * KB)
            self.assertIn("longer than its region", str(caught.exception))

    def test_a_component_is_refused_when_its_binary_does_not_fit(self):
        with tempfile.TemporaryDirectory() as folder:
            _image(Path(folder), "HYDRA_KB_CM7_APP_v0.0.1.bin", 300 * KB)
            args = (folder, "HYDRA_KB_CM7_APP", "CM7 application", "STM32H745ZIT6 (CM7)", "0x48374337", {"major": 0, "minor": 0, "patch": 1}, "0x08040000")
            with self.assertRaises(SystemExit):
                manifest.component(*args, 256 * KB, manifest.CM7_RAM)
            entry = manifest.component(*args, 384 * KB, manifest.CM7_RAM)
            self.assertEqual(entry["files"]["bin"]["size_bytes"], 300 * KB)

    def test_the_report_says_what_fits_in_256_kb(self):
        with tempfile.TemporaryDirectory() as folder:
            _image(Path(folder), "HYDRA_KB_CM7_APP_v0.0.1.bin", 100 * KB)
            _image(Path(folder), "HYDRA_KB_CM4_APP_v0.0.1.bin", 256 * KB)
            lines, fits = manifest.meta_ab_report(folder)
            self.assertTrue(fits)
            self.assertIn(f"| CM7 | {100 * KB} | {156 * KB} |", lines)
            self.assertIn("| CM4 | 262144 | 0 |", lines)
            _image(Path(folder), "HYDRA_KB_CM4_APP_v0.0.2.bin", 256 * KB + 1)
            # the newest binary of a component is the one that counts
            import os
            os.utime(Path(folder) / "HYDRA_KB_CM4_APP_v0.0.2.bin", (4_000_000_000, 4_000_000_000))
            lines, fits = manifest.meta_ab_report(folder)
            self.assertFalse(fits)
            self.assertIn("| CM4 | 262145 | -1 |", lines)
            self.assertIn("stays off", lines[-1])


if __name__ == "__main__":
    unittest.main()
