# =============================================================================
# HYDRA-UMC - tests/test_ui_budget.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The memory and bandwidth budget of the interface against the memory map of the H757 (tools/ui_budget.py). The test pins what the budget says today: with the CM7 application
slot as the profile has it the interface does not fit, with the proposed larger slot it does, 60 Hz does not fit the scan-out and 30 Hz does. Evidence: simulator (arithmetic
on the data); nothing here was measured on a board."""
import copy
import json
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
        self.assertEqual(reg["FLASH_APP_CM7"]["length"], 768 * 1024)
        self.assertEqual(reg["RAM_CM7"]["length"], 512 * 1024)
        self.assertEqual(reg["SDRAM_BANK1"]["length"], 128 * 1024 * 1024)  # the board documents; the budget takes the size from config/ui.cfg

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

    def test_the_interface_fits_the_768_kib_slot_of_the_profile_and_did_not_fit_the_old_one(self):
        n = self.numbers
        self.assertEqual(n["flash_slot_today_bytes"], 768 * 1024)
        self.assertTrue(n["flash_fits"])
        self.assertTrue(n["flash_slot_ends_inside_bank"])
        self.assertGreater(n["flash_need_bytes"], 384 * 1024)  # it would not fit the 384 KiB of the earlier layout: the larger slot is what makes it fit

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

    def test_the_size_of_the_sdram_is_a_parameter_of_the_configuration_and_pending(self):
        inputs = b.load_inputs()
        self.assertEqual(inputs["sdram_confirmed"], 0)  # the model and the size are for the owner to confirm
        self.assertEqual(self.numbers["sdram_region_bytes"], int(inputs["sdram_bytes"]))
        self.assertFalse(self.numbers["sdram_confirmed"])
        small = dict(inputs, sdram_bytes=4 * 1024 * 1024)
        self.assertFalse(b.compute(self.budget, small)["sdram_fits"])
        bigger = dict(inputs, sdram_bytes=256 * 1024 * 1024)
        self.assertTrue(b.compute(self.budget, bigger)["sdram_fits"])
        self.assertTrue(any("NOT confirmed" in line for line in b.report(self.numbers)))
        self.assertNotIn('"sdram_bytes":', json.dumps(self.budget))  # not a figure of the budget file

    def test_the_default_size_is_the_banks_the_profile_enables(self):
        import platform_profile as pp

        reg = b.regions()
        banks = pp.profile("h757_target")["new_peripherals"]["FMC"]["sdram"]["banks"]
        enabled = sum(reg[bank["region"]]["length"] for bank in banks if bank["enabled"])
        self.assertEqual(enabled, reg["SDRAM_BANK1"]["length"])  # bank 2 is optional and off by default
        self.assertEqual(int(b.load_inputs()["sdram_bytes"]), enabled)
        self.assertEqual(self.numbers["sdram_region_bytes"], enabled)

    def test_the_camera_thumbnail_fits_the_memory_and_the_spi_link(self):
        n = self.numbers
        self.assertEqual(n["thumb_buffers_bytes"], 2 * 320 * 180 * 2 + 2 * 24576)
        self.assertEqual(n["thumb_spi_bytes_per_s"], 24576 * 2)
        self.assertTrue(n["thumb_fits_spi"])
        self.assertLess(n["thumb_spi_fraction"], 0.1)
        faster = dict(b.load_inputs(), camera_thumb_fps=30)
        self.assertFalse(b.compute(self.budget, faster)["thumb_fits_spi"])  # a video does not go over that link
        slower = dict(b.load_inputs(), spi_link_clock_hz=1_000_000)
        self.assertFalse(b.compute(self.budget, slower)["thumb_fits_spi"])

    def test_a_heavier_library_or_a_bigger_reserve_is_noticed(self):
        heavier = copy.deepcopy(self.budget)
        heavier["stack"]["flash_bytes"]["value"] = 700 * 1024
        self.assertFalse(b.compute(heavier)["flash_fits"])
        hungrier = copy.deepcopy(self.budget)
        hungrier["stack"]["ram_bytes"]["value"] = 600 * 1024
        self.assertFalse(b.compute(hungrier)["ram_fits"])

    def test_the_budget_prints_and_passes_with_the_proposed_layout(self):
        self.assertEqual(b.main(), 0)
        self.assertEqual(len(b.report(self.numbers)), 7)

    def test_the_document_quotes_the_figures_of_the_tool(self):
        text = (ROOT / "docs" / "UI_STACK_H757.md").read_text(encoding="utf-8")
        n = self.numbers
        kib = lambda value: f"{value / 1024:.1f}"  # noqa: E731
        self.assertIn(f"{kib(n['tables_bytes']['texts'])} KiB", text)
        self.assertIn(f"{kib(n['tables_bytes']['screens'])} KiB", text)
        self.assertIn(f"{round(n['font_bytes'] / 1024)} KiB", text)
        self.assertIn(f"{round(n['flash_need_bytes'] / 1024)} KiB", text)
        self.assertIn(f"{n['ram_ui_bytes'] // 1024} KiB of the 512 KiB", text)
        self.assertIn(f"{n['scanout_bytes_per_s'] // 1_000_000} MB/s", text)
        self.assertIn(f"{round(n['thumb_buffers_bytes'] / 1024)} KiB of the SDRAM", text)
        self.assertIn(f"{round(n['thumb_spi_bytes_per_s'] / 1024)} KiB/s = {round(n['thumb_spi_fraction'] * 100)} %", text)
        self.assertIn("to be confirmed by the owner (pending decision)", text)
        g = n["glyphs"]
        self.assertIn(f"{g['latin']} Latin, {g['cjk_ja']} Japanese, {g['cjk_zh']} Chinese ({g['cjk_union']} distinct CJK)", text)


if __name__ == "__main__":
    unittest.main()
