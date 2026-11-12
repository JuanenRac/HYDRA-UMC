# =============================================================================
# HYDRA-UMC - tests/test_ui_budget.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The memory and bandwidth budget of the interface against the memory map of the H757 (tools/ui_budget.py). The test pins what the budget says today: with the CM7 application
slot as the profile has it the interface does not fit, with the proposed larger slot it does, 60 Hz does not fit the scan-out and 30 Hz does. Evidence: simulator (arithmetic
on the data); nothing here was measured on a board."""
import copy
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import ui_budget as b  # noqa: E402
import ui_spec as s  # noqa: E402


class BudgetTests(unittest.TestCase):
    def setUp(self):
        self.budget = b.load_budget()
        self.numbers = b.compute(self.budget)

    def test_every_figure_says_where_it_comes_from(self):
        def walk(node, path):
            if isinstance(node, dict):
                if "value" in node:
                    self.assertIn(node.get("origin"), ("data", "published", "estimate"), path)
                    self.assertTrue(node.get("note"), path)
                for key, value in node.items():
                    walk(value, f"{path}.{key}")

        walk(self.budget, "budget")
        self.assertEqual(self.budget["stack"]["license"], "MIT")

    def test_the_regions_are_those_of_the_h757_profile(self):
        reg = b.regions()
        self.assertEqual(reg["FLASH_APP_CM7"]["length"], 384 * 1024)
        self.assertEqual(reg["RAM_CM7"]["length"], 512 * 1024)
        self.assertEqual(reg["SDRAM_BANK1"]["length"], 128 * 1024 * 1024)

    def test_the_tables_are_counted_from_the_data(self):
        spec, i18n = s.load_spec(), s.load_i18n()
        tables = b.table_bytes(spec, i18n)
        floor = sum(len(e[lang].encode("utf-8")) for e in i18n["keys"].values() for lang in s.LANGUAGES)
        self.assertGreater(tables["texts"], floor)
        self.assertGreater(tables["screens"], 0)
        self.assertLess(tables["texts"], 64 * 1024)  # the eight languages stay a small part of the flash

    def test_the_glyphs_are_the_characters_of_the_texts(self):
        glyphs = b.glyph_counts(s.load_i18n())
        self.assertGreater(glyphs["cjk_ja"], 100)
        self.assertGreater(glyphs["cjk_zh"], 100)
        self.assertGreaterEqual(glyphs["cjk_union"], max(glyphs["cjk_ja"], glyphs["cjk_zh"]))
        self.assertLessEqual(glyphs["cjk_union"], glyphs["cjk_ja"] + glyphs["cjk_zh"])
        self.assertGreater(glyphs["latin"], 50)

    def test_the_interface_does_not_fit_the_application_slot_of_today_but_fits_the_proposed_one(self):
        n = self.numbers
        self.assertFalse(n["flash_fits_today"], "if this starts to fit, the proposal of a larger slot is no longer needed: update the documents")
        self.assertTrue(n["flash_fits_proposed"])
        self.assertTrue(n["flash_proposed_ends_inside_bank"])
        self.assertGreater(n["flash_slot_proposed_bytes"], n["flash_slot_today_bytes"])

    def test_the_fonts_live_outside_the_internal_flash(self):
        n = self.numbers
        self.assertGreater(n["font_bytes"], 100 * 1024)  # they would not fit beside the library
        self.assertTrue(n["assets_fit_nor"])
        self.assertLess(n["assets_bytes"], 16 * 1024 * 1024 // 4)
        self.assertNotIn(n["font_bytes"], (n["flash_ui_bytes"],))

    def test_ram_sdram_and_the_scanout_fit_at_30_hz(self):
        n = self.numbers
        self.assertTrue(n["ram_fits"])
        self.assertTrue(n["sdram_fits"])
        self.assertTrue(n["scanout_fits"])
        self.assertEqual(n["frame_bytes"], 1280 * 720 * 2)
        self.assertEqual(n["sdram_peak_bytes_per_s"], 400_000_000)

    def test_sixty_hertz_does_not_fit_the_scanout_budget(self):
        faster = copy.deepcopy(self.budget)
        faster["display"]["refresh_hz"]["value"] = 60
        self.assertFalse(b.compute(faster)["scanout_fits"])

    def test_a_heavier_library_or_a_bigger_reserve_is_noticed(self):
        heavier = copy.deepcopy(self.budget)
        heavier["stack"]["flash_bytes"]["value"] = 700 * 1024
        self.assertFalse(b.compute(heavier)["flash_fits_proposed"])
        hungrier = copy.deepcopy(self.budget)
        hungrier["stack"]["ram_bytes"]["value"] = 600 * 1024
        self.assertFalse(b.compute(hungrier)["ram_fits"])

    def test_the_budget_prints_and_passes_with_the_proposed_layout(self):
        self.assertEqual(b.main(), 0)
        self.assertEqual(len(b.report(self.numbers)), 6)

    def test_the_document_quotes_the_figures_of_the_tool(self):
        text = (ROOT / "docs" / "UI_STACK_H757.md").read_text(encoding="utf-8")
        n = self.numbers
        kib = lambda value: f"{value / 1024:.1f}"  # noqa: E731
        self.assertIn(f"{kib(n['tables_bytes']['texts'])} KiB", text)
        self.assertIn(f"{kib(n['tables_bytes']['screens'])} KiB", text)
        self.assertIn(f"{round(n['font_bytes'] / 1024)} KiB", text)
        self.assertIn(f"{n['flash_need_bytes'] // 1024} KiB", text)
        self.assertIn(f"{n['ram_ui_bytes'] // 1024} KiB of the 512 KiB", text)
        self.assertIn(f"{n['scanout_bytes_per_s'] // 1_000_000} MB/s", text)
        g = n["glyphs"]
        self.assertIn(f"{g['latin']} Latin, {g['cjk_ja']} Japanese, {g['cjk_zh']} Chinese ({g['cjk_union']} distinct CJK)", text)


if __name__ == "__main__":
    unittest.main()
