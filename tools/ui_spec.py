#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/ui_spec.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Reads and checks the data of the on-board interface: the screens, the signals they are bound to and the texts in eight languages.

    python tools/ui_spec.py check              exit 1 when the screens, the texts or their links to the contract have a problem
    python tools/ui_spec.py arb <l10n folder>  exit 1 when a text that claims to be the same as one of the DSI app differs from it

The files are `src/ui_h757/spec/ui_spec.json` (closed schema `src/ui_h757/spec/ui_spec.schema.json`) and `src/ui_h757/spec/i18n.json`. What a schema cannot say is checked here: every signal,
action and text a widget names exists, the geometry fits the frame, a touch target is large enough, the STOP button is on every screen and is never gated, every
command is refused while listen-only is on, the colours keep their contrast, and every bound signal points at a message of the real contract (`docs/can_ids.json`,
`docs/bus_contract.json`) or at a contract that is declared pending. Evidence: simulator (data checks); nothing here has run on a display."""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
SPEC_PATH = ROOT / "src" / "ui_h757" / "spec" / "ui_spec.json"
SCHEMA_PATH = ROOT / "src" / "ui_h757" / "spec" / "ui_spec.schema.json"
I18N_PATH = ROOT / "src" / "ui_h757" / "spec" / "i18n.json"
LANGUAGES = ("en", "es", "de", "fr", "it", "ja", "zh", "pt")
MIN_CONTRAST = 4.5  # WCAG AA for normal text


def read_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def load_spec() -> dict:
    return read_json(SPEC_PATH)


def load_i18n() -> dict:
    return read_json(I18N_PATH)


# ---------------------------------------------------------------- the schema (the subset of JSON Schema the file uses)
def _type_ok(value: Any, kind: str) -> bool:
    if kind == "object":
        return isinstance(value, dict)
    if kind == "array":
        return isinstance(value, list)
    if kind == "string":
        return isinstance(value, str)
    if kind == "boolean":
        return isinstance(value, bool)
    if kind == "integer":
        return isinstance(value, int) and not isinstance(value, bool)
    if kind == "number":
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    return False


def _resolve(root: dict, ref: str) -> dict:
    node: Any = root
    for part in ref.lstrip("#/").split("/"):
        node = node[part]
    return node


def schema_problems(value: Any, schema: dict, root: dict | None = None, path: str = "$") -> list[str]:
    root = root or schema
    if "$ref" in schema:
        return schema_problems(value, _resolve(root, schema["$ref"]), root, path)
    found: list[str] = []
    kind = schema.get("type")
    if kind and not _type_ok(value, kind):
        return [f"{path}: expected {kind}"]
    if "enum" in schema and value not in schema["enum"]:
        found.append(f"{path}: {value!r} is not one of {schema['enum']}")
    if isinstance(value, str) and "pattern" in schema and not re.search(schema["pattern"], value):
        found.append(f"{path}: {value!r} does not match {schema['pattern']}")
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        if "minimum" in schema and value < schema["minimum"]:
            found.append(f"{path}: {value} is below {schema['minimum']}")
        if "maximum" in schema and value > schema["maximum"]:
            found.append(f"{path}: {value} is above {schema['maximum']}")
    if isinstance(value, list):
        if "minItems" in schema and len(value) < schema["minItems"]:
            found.append(f"{path}: fewer than {schema['minItems']} items")
        if "maxItems" in schema and len(value) > schema["maxItems"]:
            found.append(f"{path}: more than {schema['maxItems']} items")
        if "items" in schema:
            for index, item in enumerate(value):
                found += schema_problems(item, schema["items"], root, f"{path}[{index}]")
    if isinstance(value, dict):
        properties = schema.get("properties", {})
        for name in schema.get("required", []):
            if name not in value:
                found.append(f"{path}: {name} is missing")
        extra = schema.get("additionalProperties", True)
        for name, item in value.items():
            if name in properties:
                found += schema_problems(item, properties[name], root, f"{path}.{name}")
            elif extra is False:
                found.append(f"{path}: {name} is not a field of the schema")
            elif isinstance(extra, dict):
                found += schema_problems(item, extra, root, f"{path}.{name}")
    return found


# ---------------------------------------------------------------- contrast
def _channel(value: int) -> float:
    c = value / 255
    return c / 12.92 if c <= 0.03928 else ((c + 0.055) / 1.055) ** 2.4


def luminance(colour: str) -> float:
    r, g, b = (int(colour[i : i + 2], 16) for i in (1, 3, 5))
    return 0.2126 * _channel(r) + 0.7152 * _channel(g) + 0.0722 * _channel(b)


def contrast(first: str, second: str) -> float:
    a, b = luminance(first), luminance(second)
    high, low = max(a, b), min(a, b)
    return (high + 0.05) / (low + 0.05)


# ---------------------------------------------------------------- the contract names
def contract_names(root: Path = ROOT) -> dict[str, set[str]]:
    can = read_json(root / "docs" / "can_ids.json")
    bus = read_json(root / "docs" / "bus_contract.json")
    text = (root / "docs" / "SPI_SYSTEM.md").read_text(encoding="utf-8")
    link = read_json(root / "src" / "ui_h757" / "spec" / "jetson_link.json")
    return {
        "link": {f["name"] for f in link["frames"] if f["direction"] == "host_to_master"},
        "can_message": {m["name"] for m in can["messages"]},
        "bus_contract": set(bus["buses"]) | {"emergency_stop"},
        "spi_self": set(re.findall(r"\| `0x[0-9A-F]{2}` \| `([A-Z0-9_]+)` \|", text)),
    }


def _inside(inner: dict, outer: dict) -> bool:
    return (inner["x"] >= outer["x"] and inner["y"] >= outer["y"] and inner["x"] + inner["w"] <= outer["x"] + outer["w"]
            and inner["y"] + inner["h"] <= outer["y"] + outer["h"])


def _overlap(a: dict, b: dict) -> bool:
    return a["x"] < b["x"] + b["w"] and b["x"] < a["x"] + a["w"] and a["y"] < b["y"] + b["h"] and b["y"] < a["y"] + a["h"]


def i18n_problems(i18n: dict) -> list[str]:
    found: list[str] = []
    if tuple(i18n.get("languages", ())) != LANGUAGES:
        found.append(f"languages must be {list(LANGUAGES)}")
    for key, entry in i18n.get("keys", {}).items():
        missing = [lang for lang in LANGUAGES if not isinstance(entry.get(lang), str) or not entry[lang].strip()]
        if missing:
            found.append(f"text {key}: empty or missing in {missing}")
        extra = set(entry) - set(LANGUAGES) - {"arb"}
        if extra:
            found.append(f"text {key}: unknown fields {sorted(extra)}")
        for lang in LANGUAGES:
            text = entry.get(lang, "")
            if isinstance(text, str) and re.search(r"\{[a-zA-Z_]+\}", text):
                found.append(f"text {key} {lang}: has a placeholder; the on-board table has none")
    return found


def arb_problems(i18n: dict, folder: Path) -> list[str]:
    found: list[str] = []
    for lang in LANGUAGES:
        path = folder / f"app_{lang}.arb"
        if not path.is_file():
            found.append(f"{path} not found")
            continue
        arb = read_json(path)
        for key, entry in i18n["keys"].items():
            name = entry.get("arb")
            if name and arb.get(name) != entry[lang]:
                found.append(f"text {key} ({lang}) differs from {name} of the DSI app")
    return found


def problems(spec: dict, i18n: dict, root: Path = ROOT) -> list[str]:
    found = schema_problems(spec, read_json(root / "src" / "ui_h757" / "spec" / "ui_spec.schema.json"))
    if found:
        return found  # the rest reads fields the schema has not vouched for
    found += i18n_problems(i18n)
    texts = set(i18n["keys"])
    display = spec["display"]
    frame = {"x": 0, "y": 0, "w": display["width"], "h": display["height"]}
    signals = {s["id"]: s for s in spec["signals"]}
    actions = {a["id"]: a for a in spec["actions"]}
    if len(signals) != len(spec["signals"]):
        found.append("two signals share an id")
    if len(actions) != len(spec["actions"]):
        found.append("two actions share an id")
    names = contract_names(root)
    for sig in spec["signals"]:
        kind, _, name = sig["contract_ref"].partition(":")
        if kind in names and name not in names[kind]:
            found.append(f"signal {sig['id']}: {sig['contract_ref']} is not in the contract")
        if kind == "pending" and name not in spec["pending_contracts"]:
            found.append(f"signal {sig['id']}: pending contract {name} is not declared")
        if (sig["source"] == "jetson_spi") != sig["needs_jetson"]:
            found.append(f"signal {sig['id']}: needs_jetson must be true exactly for the signals that come from the Jetson")
        if sig["source"] == "jetson_spi" and kind not in ("link", "pending"):
            found.append(f"signal {sig['id']}: a signal of the Jetson must point at a frame of the Jetson link (link:) or at a contract declared pending")
        if sig["source"] == "stack_bus" and kind not in ("can_message", "bus_contract"):
            found.append(f"signal {sig['id']}: a stack bus signal must point at a message or a bus of the contract")
        if sig["needs_jetson"] and sig["max_age_ms"] == 0:
            found.append(f"signal {sig['id']}: a signal of the Jetson needs a freshness limit")
    for act in spec["actions"]:
        if (act["confirm"] == "none") != (act["hold_ms"] == 0):
            found.append(f"action {act['id']}: hold_ms must be set exactly when a confirmation is asked")
        if act["kind"] == "command" and act["id"] != "jog_arm" and not {"not_listen_only", "jog_armed"} & set(act["requires"]):
            found.append(f"action {act['id']}: a command must be refused while listen-only is on (directly or through the armed jog)")
        if act["kind"] == "command" and "jog_armed" not in act["requires"] and "not_listen_only" not in act["requires"]:
            found.append(f"action {act['id']}: a command without listen-only protection")
        if act["kind"] == "nav" and not act["id"].startswith("nav_"):
            found.append(f"action {act['id']}: a navigation action is named nav_<screen>")
    stop = actions.get("stop")
    if stop is None or stop["kind"] != "stop" or stop["requires"] or stop["confirm"] != "none":
        found.append("the stop action must exist, need nothing and ask for no confirmation")
    if sum(1 for a in spec["actions"] if a["kind"] == "stop") != 1:
        found.append("there must be exactly one stop action")

    seen_ids: set[str] = set()
    used_signals: set[str] = set()
    used_actions: set[str] = set()
    regions = spec["regions"]

    def check_widget(widget: dict, area: dict | None, where: str) -> None:
        wid = widget["id"]
        if wid in seen_ids:
            found.append(f"{where}: widget id {wid} is used twice")
        seen_ids.add(wid)
        kind = widget["kind"]
        for field in ("label_key", "hint_key", "unit_key"):
            if field in widget and widget[field] not in texts:
                found.append(f"{where}/{wid}: {field} {widget[field]} is not a text")
        for field in ("signal", "value_signal"):
            if field in widget:
                if widget[field] not in signals:
                    found.append(f"{where}/{wid}: {field} {widget[field]} is not a signal")
                used_signals.add(widget[field])
        if "action" in widget:
            if widget["action"] not in actions:
                found.append(f"{where}/{wid}: action {widget['action']} is not an action")
            used_actions.add(widget["action"])
        needs = {"button": ("action", "label_key"), "tab": ("action", "label_key"), "toggle": ("action", "signal", "label_key"),
                 "value": ("signal", "label_key"), "led": ("signal", "label_key"), "banner": ("signal",), "list": ("signal",),
                 "image": ("signal",), "text": ("label_key",)}[kind]
        for field in needs:
            if field not in widget:
                found.append(f"{where}/{wid}: a {kind} needs {field}")
        if kind not in ("button", "tab", "toggle", "banner") and "action" in widget:
            found.append(f"{where}/{wid}: only a button, a tab, a toggle or the banner carries an action")
        if not _inside(widget["rect"], frame):
            found.append(f"{where}/{wid}: outside the frame")
        if area is not None and not _inside(widget["rect"], area):
            found.append(f"{where}/{wid}: outside its region")
        if "action" in widget and widget["action"] in actions:
            act = actions[widget["action"]]
            rect = widget["rect"]
            if act["kind"] == "stop":
                if rect["w"] < display["stop_min_px"][0] or rect["h"] < display["stop_min_px"][1]:
                    found.append(f"{where}/{wid}: the stop button is smaller than {display['stop_min_px']}")
            elif min(rect["w"], rect["h"]) < display["min_touch_px"]:
                found.append(f"{where}/{wid}: a touch target below {display['min_touch_px']} px")
            if widget.get("hold") and act["confirm"] == "none":
                found.append(f"{where}/{wid}: marked hold but the action asks for none")
            if act["confirm"] != "none" and not widget.get("hold"):
                found.append(f"{where}/{wid}: the action asks for a long press and the widget does not say so")

    area_of = {"value": regions["status_bar"], "led": regions["status_bar"], "tab": regions["tab_bar"], "banner": regions["footer"]}
    for widget in spec["global_widgets"]:
        if widget["kind"] == "button":
            area = regions["footer"]
        elif widget["id"].startswith("status_"):
            area = regions["status_bar"]
        else:
            area = area_of.get(widget["kind"])
        check_widget(widget, area, "global")
    for first_index, first in enumerate(spec["global_widgets"]):
        for second in spec["global_widgets"][first_index + 1 :]:
            if _overlap(first["rect"], second["rect"]):
                found.append(f"global: {first['id']} overlaps {second['id']}")
    stop_buttons = [w for w in spec["global_widgets"] if w.get("action") == "stop"]
    if len(stop_buttons) != 1:
        found.append("the STOP button must be one global widget (on every screen)")
    for screen in spec["screens"]:
        if screen["title_key"] not in texts:
            found.append(f"screen {screen['id']}: title {screen['title_key']} is not a text")
        for widget in screen["widgets"]:
            check_widget(widget, regions["content"], f"screen {screen['id']}")
            if widget.get("action") == "stop":
                found.append(f"screen {screen['id']}: the stop button belongs to the global widgets, not to a screen")
        for first_index, first in enumerate(screen["widgets"]):
            for second in screen["widgets"][first_index + 1 :]:
                if _overlap(first["rect"], second["rect"]):
                    found.append(f"screen {screen['id']}: {first['id']} overlaps {second['id']}")
    screen_ids = [s["id"] for s in spec["screens"]]
    if len(set(screen_ids)) != len(screen_ids):
        found.append("two screens share an id")
    nav_targets = {a["id"][4:] for a in spec["actions"] if a["kind"] == "nav"}
    if nav_targets != set(screen_ids):
        found.append(f"the navigation actions {sorted(nav_targets)} and the screens {sorted(screen_ids)} must match")
    tabs = {w["action"][4:] for w in spec["global_widgets"] if w["kind"] == "tab"}
    modal = {sc["id"] for sc in spec["screens"] if sc.get("modal")}
    if tabs != set(screen_ids) - modal:
        found.append("every screen that is not modal needs exactly one tab, and a modal screen has none")
    for unused in sorted(set(signals) - used_signals):
        found.append(f"signal {unused} is bound to no widget")
    for unused in sorted(set(actions) - used_actions):
        found.append(f"action {unused} is carried by no widget")

    theme = spec["theme"]
    for text, ground in (("text", "background"), ("text_muted", "background"), ("text", "surface"), ("ok_text", "ok"), ("warn_text", "warn"),
                         ("danger_text", "danger"), ("stop_text", "stop"), ("disabled_text", "disabled")):
        if contrast(theme[text], theme[ground]) < MIN_CONTRAST:
            found.append(f"theme: {text} on {ground} has a contrast of {contrast(theme[text], theme[ground]):.2f}, below {MIN_CONTRAST}")
    return found


def main(argv: list[str]) -> int:
    command = argv[0] if argv else "check"
    i18n = load_i18n()
    if command == "check":
        found = problems(load_spec(), i18n)
    elif command == "arb" and len(argv) == 2:
        found = arb_problems(i18n, Path(argv[1]))
    else:
        print(__doc__)
        return 2
    for line in found:
        print("PROBLEM:", line)
    print("ui_spec:", "FAIL" if found else "PASS")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
