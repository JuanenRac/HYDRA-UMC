# =============================================================================
# HYDRA-UMC - tests/test_ui_render.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The reference rendering (tools/ui_render.py): every screen in every language and in the states that matter fits its boxes, the pictures have the size of the frame, and a
text that cannot fit is reported. It needs Pillow and a font for each script; with neither it skips (the CI of the interface installs both). Evidence: simulator."""
import copy
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

try:
    import ui_render as r
    from PIL import Image
except ImportError:  # no Pillow
    r = None

import ui_model as m  # noqa: E402
import ui_spec as s  # noqa: E402


def fonts_present() -> bool:
    if r is None:
        return False
    try:
        for language in s.LANGUAGES:
            r.find_font(language)
    except r.FontMissing:
        return False
    return True


@unittest.skipUnless(fonts_present(), "Pillow or a font for one of the eight languages is missing: the CI of the interface has them")
class RenderTests(unittest.TestCase):
    def test_every_screen_in_every_language_and_scene_fits_its_boxes(self):
        folder = Path(tempfile.mkdtemp(prefix="ui_render_"))
        written = r.render_set(folder, s.LANGUAGES, r.SCENES, m.SCREENS)
        self.assertEqual(len(written), 8 * len(r.SCENES) * 8)
        with Image.open(written[0]) as image:
            self.assertEqual(image.size, (1280, 720))
        for path in written:
            path.unlink()
        folder.rmdir()

    def test_the_scenes_reach_the_states_they_are_named_for(self):
        self.assertEqual(m.MODES[r.scene_state("normal").mode], "normal")
        self.assertEqual(m.MODES[r.scene_state("degraded").mode], "degraded")
        self.assertEqual(m.MODES[r.scene_state("stopped").mode], "stopped")
        self.assertEqual(m.MODES[r.scene_state("alarm").mode], "alarm")
        self.assertTrue(r.scene_state("listen_only").listen_only)
        with self.assertRaises(ValueError):
            r.scene_state("nope")

    def test_a_text_that_cannot_fit_is_reported(self):
        spec, i18n = s.load_spec(), copy.deepcopy(s.load_i18n())
        i18n["keys"]["nav.control"]["en"] = "A very long name for a tab that cannot possibly fit in one hundred and sixty pixels of width at any size"
        renderer = r.Renderer(spec, i18n, "en")
        renderer.render(r.scene_state("normal"), "dashboard")
        self.assertTrue(any("tab_control" in line for line in renderer.overflows))

    def test_wrapping_breaks_cjk_anywhere_and_latin_at_spaces(self):
        renderer = r.Renderer(s.load_spec(), s.load_i18n(), "ja")
        font = renderer.fonts.get(20)
        lines = r.wrap("Jetson が応答しません。ビジョン、AI、サーバーは停止中です。", font, 300)
        self.assertGreater(len(lines), 1)
        self.assertTrue(all(font.getlength(line) <= 300 for line in lines))
        self.assertIn("Jetson", lines[0])


if __name__ == "__main__":
    unittest.main()
