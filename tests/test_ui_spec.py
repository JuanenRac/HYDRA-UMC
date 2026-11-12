# =============================================================================
# HYDRA-UMC - tests/test_ui_spec.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The screens of the interface as data (src/ui_h757/spec): the closed schema, the links to the contract, the eight languages, the touch targets and the generated C tables.
Evidence: simulator (data checks); nothing ran on a display."""
import copy
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import ui_gen  # noqa: E402
import ui_model as m  # noqa: E402
import ui_spec as s  # noqa: E402

DSI_L10N = ROOT.parent / "HYDRA-UMC-DSI" / "lib" / "l10n"


class SpecTests(unittest.TestCase):
    def setUp(self):
        self.spec = s.load_spec()
        self.i18n = s.load_i18n()

    def problems(self, spec=None, i18n=None):
        return s.problems(spec or self.spec, i18n or self.i18n)

    def test_the_files_are_clean(self):
        self.assertEqual(self.problems(), [])
        self.assertEqual(s.main(["check"]), 0)

    def test_the_schema_is_closed_everywhere(self):
        schema = s.read_json(s.SCHEMA_PATH)

        def objects(node, path="$"):
            if isinstance(node, dict):
                if node.get("type") == "object" and "properties" in node:
                    yield path, node
                for key, value in node.items():
                    yield from objects(value, f"{path}.{key}")
            elif isinstance(node, list):
                for index, value in enumerate(node):
                    yield from objects(value, f"{path}[{index}]")

        found = list(objects(schema))
        self.assertGreaterEqual(len(found), 9)
        for path, node in found:
            self.assertIs(node.get("additionalProperties"), False, path)

    def test_an_invented_field_is_refused_at_every_level(self):
        for mutate in (lambda d: d.update(extra=1), lambda d: d["display"].update(extra=1), lambda d: d["signals"][0].update(extra=1),
                       lambda d: d["actions"][0].update(extra=1), lambda d: d["screens"][0].update(extra=1), lambda d: d["screens"][0]["widgets"][0].update(extra=1),
                       lambda d: d["screens"][0]["widgets"][0]["rect"].update(extra=1), lambda d: d["theme"].update(extra="#000000")):
            spec = copy.deepcopy(self.spec)
            mutate(spec)
            self.assertTrue(any("not a field" in p for p in self.problems(spec)))

    def test_the_stop_button_is_global_large_and_never_gated(self):
        stops = [w for w in self.spec["global_widgets"] if w.get("action") == "stop"]
        self.assertEqual(len(stops), 1)
        self.assertGreaterEqual(stops[0]["rect"]["w"], self.spec["display"]["stop_min_px"][0])
        self.assertGreaterEqual(stops[0]["rect"]["h"], self.spec["display"]["stop_min_px"][1])
        action = next(a for a in self.spec["actions"] if a["id"] == "stop")
        self.assertEqual((action["requires"], action["confirm"], action["hold_ms"]), ([], "none", 0))
        spec = copy.deepcopy(self.spec)
        next(a for a in spec["actions"] if a["id"] == "stop")["requires"] = ["normal"]
        self.assertTrue(any("stop action" in p for p in self.problems(spec)))
        spec = copy.deepcopy(self.spec)
        spec["screens"][0]["widgets"].append({"id": "second_stop", "kind": "button", "label_key": "stop.label", "action": "stop", "rect": {"x": 24, "y": 540, "w": 272, "h": 64}})
        self.assertTrue(any("belongs to the global widgets" in p for p in self.problems(spec)))

    def test_a_small_touch_target_is_refused(self):
        spec = copy.deepcopy(self.spec)
        spec["global_widgets"][-1]["rect"]["h"] = 40
        self.assertTrue(any("touch target below" in p for p in self.problems(spec)))
        spec = copy.deepcopy(self.spec)
        next(w for w in spec["global_widgets"] if w["id"] == "stop_button")["rect"]["w"] = 200
        self.assertTrue(any("stop button is smaller" in p for p in self.problems(spec)))

    def test_overlaps_and_widgets_outside_their_region_are_refused(self):
        spec = copy.deepcopy(self.spec)
        spec["screens"][0]["widgets"][1]["rect"] = dict(spec["screens"][0]["widgets"][0]["rect"])
        self.assertTrue(any("overlaps" in p for p in self.problems(spec)))
        spec = copy.deepcopy(self.spec)
        spec["screens"][0]["widgets"][0]["rect"]["y"] = 600
        self.assertTrue(any("outside its region" in p for p in self.problems(spec)))

    def test_every_command_is_refused_while_listen_only_is_on(self):
        spec = copy.deepcopy(self.spec)
        next(a for a in spec["actions"] if a["id"] == "jog_arm")["requires"] = ["normal"]
        spec["actions"].append({"id": "extra", "kind": "command", "confirm": "none", "hold_ms": 0, "requires": ["normal"], "note": ""})
        self.assertTrue(any("listen-only" in p for p in self.problems(spec)))

    def test_a_signal_must_point_at_the_real_contract_or_a_declared_pending_one(self):
        spec = copy.deepcopy(self.spec)
        next(x for x in spec["signals"] if x["id"] == "estop.latched")["contract_ref"] = "can_message:NO_SUCH_MESSAGE"
        self.assertTrue(any("not in the contract" in p for p in self.problems(spec)))
        spec = copy.deepcopy(self.spec)
        del spec["pending_contracts"]["jetson_status"]
        self.assertTrue(any("not declared" in p for p in self.problems(spec)))
        spec = copy.deepcopy(self.spec)
        next(x for x in spec["signals"] if x["id"] == "jetson.cpu_load_pct")["contract_ref"] = "local"
        self.assertTrue(any("must be pending" in p for p in self.problems(spec)))

    def test_the_bound_messages_are_those_of_the_contract(self):
        names = s.contract_names()
        self.assertIn("ESTOP_STATE", names["can_message"])
        self.assertIn("HEALTH_FRAME", names["can_message"])
        self.assertIn("stack_a", names["bus_contract"])
        self.assertIn("STOP_REQUEST", names["spi_self"])  # the table of docs/SPI_SYSTEM.md is read, not a copy
        refs = {x["contract_ref"] for x in self.spec["signals"]}
        self.assertTrue({"can_message:ESTOP_STATE", "can_message:HEALTH_FRAME", "bus_contract:stack_a"} <= refs)

    def test_the_signals_of_the_jetson_are_the_ones_that_need_it_and_carry_a_freshness_limit(self):
        for sig in self.spec["signals"]:
            self.assertEqual(sig["source"] == "jetson_spi", sig["needs_jetson"], sig["id"])
            if sig["needs_jetson"]:
                self.assertGreater(sig["max_age_ms"], 0, sig["id"])

    def test_what_works_without_the_jetson_is_not_bound_to_it(self):
        by_id = {x["id"]: x for x in self.spec["signals"]}
        for screen in self.spec["screens"]:
            if screen["id"] in ("control", "position", "alarms", "settings"):
                self.assertFalse(screen["needs_jetson"], screen["id"])
                for w in screen["widgets"]:
                    for field in ("signal", "value_signal"):
                        if field in w:
                            self.assertFalse(by_id[w[field]]["needs_jetson"], (screen["id"], w["id"]))
        self.assertTrue(next(x for x in self.spec["screens"] if x["id"] == "camera")["needs_jetson"])

    def test_the_colours_keep_their_contrast_and_a_bad_pair_is_refused(self):
        self.assertGreaterEqual(s.contrast("#FFFFFF", "#000000"), 20.9)
        spec = copy.deepcopy(self.spec)
        spec["theme"]["stop"] = "#E57373"
        self.assertTrue(any("stop_text on stop" in p for p in self.problems(spec)))
        for text, ground in (("text", "background"), ("stop_text", "stop"), ("warn_text", "warn"), ("danger_text", "danger"), ("ok_text", "ok")):
            self.assertGreaterEqual(s.contrast(self.spec["theme"][text], self.spec["theme"][ground]), 4.5, text)

    def test_the_action_ids_start_with_those_of_the_view_model(self):
        ids = [a["id"] for a in self.spec["actions"]]
        self.assertEqual(tuple(ids[: len(m.ACTIONS)]), m.ACTIONS)
        self.assertEqual({a[4:] for a in ids if a.startswith("nav_")}, set(m.SCREENS))
        self.assertEqual([sc["id"] for sc in self.spec["screens"]], list(m.SCREENS))

    def test_the_panel_is_landscape_only_and_the_orientation_cannot_be_portrait(self):
        self.assertEqual(self.spec["display"]["orientation"], "landscape")
        self.assertEqual(self.spec["display"]["width"] > self.spec["display"]["height"], True)
        spec = copy.deepcopy(self.spec)
        spec["display"]["orientation"] = "portrait"
        self.assertTrue(self.problems(spec))

    def test_the_pin_pad_is_a_modal_screen_opened_by_the_banner_and_never_gates_stop(self):
        unlock = next(sc for sc in self.spec["screens"] if sc["id"] == "unlock")
        self.assertTrue(unlock["modal"])
        self.assertNotIn("tab_unlock", {w["id"] for w in self.spec["global_widgets"]})
        banner = next(w for w in self.spec["global_widgets"] if w["id"] == "banner")
        self.assertEqual(banner["action"], "nav_unlock")
        self.assertGreaterEqual(min(banner["rect"]["w"], banner["rect"]["h"]), self.spec["display"]["min_touch_px"])
        digits = {w["action"] for w in unlock["widgets"] if w.get("action", "").startswith("pin_digit_")}
        self.assertEqual(digits, {f"pin_digit_{n}" for n in range(10)})
        by_id = {a["id"]: a for a in self.spec["actions"]}
        for name in digits | {"pin_clear", "pin_enter"}:
            self.assertEqual((by_id[name]["kind"], by_id[name]["requires"]), ("local", []))
        self.assertEqual(by_id["stop"]["requires"], [])  # the PIN is never asked for the stop
        spec = copy.deepcopy(self.spec)
        next(sc for sc in spec["screens"] if sc["id"] == "unlock")["modal"] = False
        self.assertTrue(any("tab" in p for p in self.problems(spec)))

    def test_the_camera_screen_shows_a_still_thumbnail_that_needs_the_jetson(self):
        camera = next(sc for sc in self.spec["screens"] if sc["id"] == "camera")
        image = next(w for w in camera["widgets"] if w["kind"] == "image")
        self.assertEqual(image["signal"], "jetson.camera_snapshot")
        sig = next(x for x in self.spec["signals"] if x["id"] == "jetson.camera_snapshot")
        self.assertEqual((sig["type"], sig["needs_jetson"], sig["contract_ref"]), ("image", True, "pending:jetson_snapshot"))
        self.assertIn("shows it", self.spec["pending_contracts"]["jetson_snapshot"])

    def test_the_holds_of_the_actions_match_the_configuration(self):
        cfg = m.load_config()
        by_id = {a["id"]: a for a in self.spec["actions"]}
        self.assertEqual(by_id["release"]["hold_ms"], cfg["release_hold_ms"])
        self.assertEqual(by_id["toggle_listen_only"]["hold_ms"], cfg["listen_off_hold_ms"])


class TextTests(unittest.TestCase):
    def setUp(self):
        self.spec = s.load_spec()
        self.i18n = s.load_i18n()

    def test_every_key_is_in_the_eight_languages(self):
        self.assertEqual(s.i18n_problems(self.i18n), [])
        self.assertEqual(list(self.i18n["languages"]), ["en", "es", "de", "fr", "it", "ja", "zh", "pt"])
        for key, entry in self.i18n["keys"].items():
            for lang in s.LANGUAGES:
                self.assertTrue(entry[lang].strip(), (key, lang))

    def test_a_missing_language_or_an_empty_text_is_refused(self):
        broken = copy.deepcopy(self.i18n)
        del broken["keys"]["nav.control"]["pt"]
        self.assertTrue(any("nav.control" in p for p in s.i18n_problems(broken)))
        broken = copy.deepcopy(self.i18n)
        broken["keys"]["nav.control"]["ja"] = "  "
        self.assertTrue(any("nav.control" in p for p in s.i18n_problems(broken)))

    def test_no_text_is_unused_and_every_text_the_core_picks_exists(self):
        used = set()
        for widget in self.spec["global_widgets"] + [w for sc in self.spec["screens"] for w in sc["widgets"]]:
            used |= {widget[f] for f in ("label_key", "hint_key", "unit_key") if f in widget}
        used |= {sc["title_key"] for sc in self.spec["screens"]}
        used |= m.core_text_keys()
        self.assertEqual(set(self.i18n["keys"]) - used, set(), "texts that nothing uses")
        self.assertEqual(m.core_text_keys() - set(self.i18n["keys"]), set(), "texts the core picks that do not exist")

    def test_the_portuguese_is_the_european_one(self):
        pt = {k: v["pt"] for k, v in self.i18n["keys"].items()}
        text = " ".join(pt.values()).lower()
        for brazilian in ("tela", "celular", "arquivo", "salvar", "carregando"):
            self.assertNotIn(brazilian, text)
        self.assertEqual(pt["nav.camera"], "Câmara")
        self.assertEqual(pt["nav.settings"], "Definições")

    def test_the_texts_that_claim_to_be_those_of_the_dsi_app_are(self):
        if not DSI_L10N.is_dir():
            self.skipTest("the DSI app is not next to this repository")
        self.assertEqual(s.arb_problems(self.i18n, DSI_L10N), [])

    def test_a_text_that_differs_from_the_app_one_is_found(self):
        if not DSI_L10N.is_dir():
            self.skipTest("the DSI app is not next to this repository")
        broken = copy.deepcopy(self.i18n)
        broken["keys"]["nav.control"]["de"] = "Bedienung"
        self.assertTrue(any("nav.control" in p for p in s.arb_problems(broken, DSI_L10N)))


class GeneratedTests(unittest.TestCase):
    def test_the_c_tables_are_what_the_data_says(self):
        self.assertEqual(ui_gen.main(["--check"]), 0)
        for path, text in ui_gen.outputs().items():
            self.assertEqual(path.read_bytes().decode("utf-8").replace("\r\n", "\n"), text, path.name)

    def test_the_generated_text_table_has_every_key_and_language(self):
        text = (ROOT / "src" / "ui_h757" / "core" / "ui_texts.h").read_text(encoding="utf-8")
        i18n = s.load_i18n()
        self.assertIn(f"UI_T_COUNT = {len(i18n['keys'])}", text)
        self.assertIn("UI_LANG_PT = 7", text)
        self.assertIn("Definições", text)
        self.assertIn("ダッシュボード", text)

    def test_the_generated_config_header_is_the_file(self):
        cfg = m.load_config()
        text = (ROOT / "src" / "ui_h757" / "core" / "ui_config.h").read_text(encoding="utf-8")
        for name in m.CONFIG_FIELDS:
            self.assertIn(f"{cfg[name]}u, /* {name} */", text)


if __name__ == "__main__":
    unittest.main()
