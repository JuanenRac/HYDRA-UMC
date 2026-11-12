#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/ui_budget.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The memory and bandwidth budget of the interface on the STM32H757, computed from data.

    python tools/ui_budget.py          print the budget and exit 1 when the proposed layout does not fit

What is computed: the bytes of the generated tables (the texts in eight languages and the screens, counted from the data), the glyphs the fonts need (the distinct characters
of the texts, per script), the framebuffers and the scan-out bandwidth against the SDRAM, and the flash and RAM of the interface against the regions of
`docs/platform_profiles.json` (profile h757_target). The stack figures come from `src/ui_h757/budget.json`, where each says whether it is computed, published by the project of the library or
an estimate. Two layouts are checked: the CM7 application slot as the profile has it today, and the proposed larger one. Nothing here was measured on a board."""
from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ui_spec  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
WIDGET_BYTES = 20  # ui_widget_t of src/ui_h757/core/ui_screens.h on a 32-bit target
SIGNAL_BYTES = 6
POINTER_BYTES = 4
SCREEN_BYTES = 8
FLASH_BANK_END = 0x08100000
MARGIN = 0.10  # the interface must leave this share of its region free


def load_budget() -> dict:
    return json.loads((ROOT / "src" / "ui_h757" / "budget.json").read_text(encoding="utf-8"))


def regions() -> dict[str, dict]:
    profile = json.loads((ROOT / "docs" / "platform_profiles.json").read_text(encoding="utf-8"))["profiles"]["h757_target"]
    return {r["name"]: r for r in profile["memory_map"]["regions"]}


def table_bytes(spec: dict, i18n: dict) -> dict[str, int]:
    texts = sum(len(entry[lang].encode("utf-8")) + 1 for entry in i18n["keys"].values() for lang in ui_spec.LANGUAGES)
    pointers = len(i18n["keys"]) * len(ui_spec.LANGUAGES) * POINTER_BYTES
    widgets = len(spec["global_widgets"]) + sum(len(s["widgets"]) for s in spec["screens"])
    return {"texts": texts + pointers, "screens": widgets * WIDGET_BYTES + len(spec["screens"]) * SCREEN_BYTES + len(spec["signals"]) * SIGNAL_BYTES}


def glyph_counts(i18n: dict) -> dict[str, int]:
    latin: set[str] = set()
    ja: set[str] = set()
    zh: set[str] = set()
    for entry in i18n["keys"].values():
        for lang in ui_spec.LANGUAGES:
            target = ja if lang == "ja" else zh if lang == "zh" else latin
            target.update(ch for ch in entry[lang] if not ch.isspace())
    cjk_only_ja = {ch for ch in ja if ord(ch) > 0x2E80}
    cjk_only_zh = {ch for ch in zh if ord(ch) > 0x2E80}
    shared = cjk_only_ja & cjk_only_zh
    latin_all = latin | {ch for ch in ja | zh if ord(ch) <= 0x2E80}
    return {"latin": len(latin_all), "cjk_ja": len(cjk_only_ja), "cjk_zh": len(cjk_only_zh), "cjk_union": len(cjk_only_ja | cjk_only_zh), "cjk_shared": len(shared)}


def font_bytes(counts: dict[str, int], fonts: dict) -> int:
    total = 0
    for px in fonts["sizes_px"]:
        full = (px * px * fonts["bits_per_pixel"] + 7) // 8
        half = ((px // 2) * px * fonts["bits_per_pixel"] + 7) // 8
        total += counts["cjk_union"] * full + counts["latin"] * half
    return total


def compute(budget: dict | None = None) -> dict:
    budget = budget or load_budget()
    spec, i18n = ui_spec.load_spec(), ui_spec.load_i18n()
    reg = regions()
    tables = table_bytes(spec, i18n)
    glyphs = glyph_counts(i18n)
    fonts = font_bytes(glyphs, budget["fonts"])
    disp = budget["display"]
    pixels = disp["width"] * disp["height"]
    frame = pixels * disp["layer_bytes_per_pixel"]["value"]
    sd = budget["sdram"]
    peak = sd["sdclk_hz"]["value"] * sd["bus_bits"]["value"] // 8
    sustained = peak * sd["sustained_fraction"]["value"]
    scanout = frame * disp["refresh_hz"]["value"]
    render_buffer = int(frame * budget["stack"]["render_buffer_fraction"]["value"])
    core = budget["measured_limits"]["ui_core"]
    flash_ui = budget["stack"]["flash_bytes"]["value"] + core["flash_bytes_max"] + tables["texts"] + tables["screens"]
    flash_reserve = budget["internal_flash"]["cm7_app_reserve_bytes"]["value"]
    slot_today = reg["FLASH_APP_CM7"]["length"]
    slot_proposed = budget["internal_flash"]["proposed_cm7_app_slot_bytes"]
    ram_ui = budget["stack"]["ram_bytes"]["value"] + render_buffer + budget["ram"]["axi_core_state_bytes"]["value"] + budget["ram"]["task_stack_bytes"]["value"]
    assets = fonts + budget["external_assets"]["images_bytes"]["value"]
    sdram_used = disp["framebuffers"]["value"] * frame + assets
    return {
        "tables_bytes": tables,
        "glyphs": glyphs,
        "font_bytes": fonts,
        "frame_bytes": frame,
        "framebuffers_bytes": disp["framebuffers"]["value"] * frame,
        "render_buffer_bytes": render_buffer,
        "sdram_peak_bytes_per_s": peak,
        "sdram_sustained_bytes_per_s": int(sustained),
        "scanout_bytes_per_s": scanout,
        "scanout_fraction": scanout / sustained,
        "flash_ui_bytes": flash_ui,
        "flash_need_bytes": flash_ui + flash_reserve,
        "flash_slot_today_bytes": slot_today,
        "flash_slot_proposed_bytes": slot_proposed,
        "flash_fits_today": flash_ui + flash_reserve <= slot_today * (1 - MARGIN),
        "flash_fits_proposed": flash_ui + flash_reserve <= slot_proposed * (1 - MARGIN),
        "flash_proposed_ends_inside_bank": reg["FLASH_APP_CM7"]["origin"] + slot_proposed <= FLASH_BANK_END,
        "ram_ui_bytes": ram_ui,
        "ram_region_bytes": reg["RAM_CM7"]["length"],
        "ram_fits": ram_ui <= reg["RAM_CM7"]["length"] * (1 - MARGIN),
        "assets_bytes": assets,
        "assets_fit_nor": assets <= budget["external_assets"]["nor_flash_bytes"],
        "sdram_used_bytes": sdram_used,
        "sdram_region_bytes": reg["SDRAM_BANK1"]["length"],
        "sdram_fits": sdram_used <= reg["SDRAM_BANK1"]["length"] * (1 - MARGIN),
        "scanout_fits": scanout <= sustained * sd["scanout_max_fraction"]["value"],
    }


def report(numbers: dict) -> list[str]:
    kib = lambda value: f"{value / 1024:.1f} KiB"  # noqa: E731
    return [
        f"generated tables: texts {kib(numbers['tables_bytes']['texts'])}, screens {kib(numbers['tables_bytes']['screens'])}",
        f"glyphs: latin {numbers['glyphs']['latin']}, ja {numbers['glyphs']['cjk_ja']}, zh {numbers['glyphs']['cjk_zh']}, union of the CJK {numbers['glyphs']['cjk_union']}; fonts {kib(numbers['font_bytes'])}",
        f"internal flash: interface {kib(numbers['flash_ui_bytes'])}, with the application reserve {kib(numbers['flash_need_bytes'])}; slot today {kib(numbers['flash_slot_today_bytes'])} "
        f"({'fits' if numbers['flash_fits_today'] else 'DOES NOT FIT'}), proposed {kib(numbers['flash_slot_proposed_bytes'])} ({'fits' if numbers['flash_fits_proposed'] else 'DOES NOT FIT'})",
        f"AXI SRAM: interface {kib(numbers['ram_ui_bytes'])} of {kib(numbers['ram_region_bytes'])} ({'fits' if numbers['ram_fits'] else 'DOES NOT FIT'})",
        f"SDRAM: framebuffers {kib(numbers['framebuffers_bytes'])}, assets {kib(numbers['assets_bytes'])}; used {kib(numbers['sdram_used_bytes'])} of {kib(numbers['sdram_region_bytes'])}",
        f"SDRAM bandwidth: peak {numbers['sdram_peak_bytes_per_s'] / 1e6:.0f} MB/s, sustained {numbers['sdram_sustained_bytes_per_s'] / 1e6:.0f} MB/s, scan-out "
        f"{numbers['scanout_bytes_per_s'] / 1e6:.0f} MB/s = {numbers['scanout_fraction'] * 100:.0f} % ({'fits' if numbers['scanout_fits'] else 'DOES NOT FIT'})",
    ]


def main() -> int:
    numbers = compute()
    for line in report(numbers):
        print(line)
    ok = numbers["flash_fits_proposed"] and numbers["ram_fits"] and numbers["sdram_fits"] and numbers["scanout_fits"] and numbers["assets_fit_nor"]
    print("ui_budget:", "PASS (with the proposed flash layout)" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
