#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/ui_render.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Reference rendering of the screens of the interface on the PC: the data of src/ui_h757/spec and a state of the view-model (tools/ui_model.py) become a picture of the 1280x720 frame.

    python tools/ui_render.py <folder> [--scene normal|degraded|stopped|alarm|listen_only] [--language en] [--screen dashboard] [--all]

This is NOT the output of the library of the firmware: it draws the same widgets, positions, texts and colours with Pillow, so a screen that overflows, a text that does not fit its
box in some language or a state that reads wrong is seen before a display exists. The picture the firmware draws is a later phase and will be compared with it. Evidence: simulator.
Fonts: the first of the candidates below that exists, or the file named by HYDRA_UI_FONT_LATIN / HYDRA_UI_FONT_CJK."""
from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ui_model as model  # noqa: E402
import ui_spec  # noqa: E402

LATIN_FONTS = ("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "C:/Windows/Fonts/DejaVuSans.ttf", "C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf", "/System/Library/Fonts/Helvetica.ttc")
CJK_FONTS = {
    "ja": ("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc#0", "C:/Windows/Fonts/YuGothR.ttc#0", "C:/Windows/Fonts/meiryo.ttc#0"),
    "zh": ("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc#2", "C:/Windows/Fonts/msyh.ttc#0", "C:/Windows/Fonts/simsun.ttc#0"),
}
SCENES = ("normal", "degraded", "stopped", "alarm", "listen_only")
BASE_SIZE = {"label": 20, "value": 32, "button": 28, "banner": 24, "tab": 22, "note": 22}


class FontMissing(RuntimeError):
    pass


def find_font(language: str) -> tuple[str, int]:
    override = os.environ.get("HYDRA_UI_FONT_CJK" if language in CJK_FONTS else "HYDRA_UI_FONT_LATIN")
    candidates = (override,) if override else (CJK_FONTS[language] if language in CJK_FONTS else LATIN_FONTS)
    for candidate in candidates:
        path, _, index = candidate.partition("#")
        if Path(path).is_file():
            return path, int(index or 0)
    raise FontMissing(f"no font for {language}: install one of {candidates} or set HYDRA_UI_FONT_LATIN / HYDRA_UI_FONT_CJK")


class Fonts:
    def __init__(self, language: str):
        self.path, self.index = find_font(language)
        self.cache: dict[int, ImageFont.FreeTypeFont] = {}

    def get(self, size: int) -> ImageFont.FreeTypeFont:
        if size not in self.cache:
            self.cache[size] = ImageFont.truetype(self.path, size, index=self.index)
        return self.cache[size]


def wrap(text: str, font: ImageFont.FreeTypeFont, width: int) -> list[str]:
    """Greedy wrapping: a word of the scripts that have spaces stays whole, a character of Chinese or Japanese may start a line."""
    lines: list[str] = []
    current = ""
    for token in re.findall(r"[^\s⺀-￿]+|\s+|[⺀-￿]", text):
        trial = current + token
        if font.getlength(trial.rstrip()) <= width or not current.strip():
            current = trial
        else:
            lines.append(current.rstrip())
            current = token.lstrip()
    if current.strip():
        lines.append(current.rstrip())
    return lines


def fit(text: str, fonts: Fonts, width: int, height: int, base: int, minimum: int) -> tuple[int, list[str]] | None:
    """The largest size from `base` down to `minimum` at which the text, wrapped, fits the box; None when it does not fit even at the minimum."""
    for size in range(base, minimum - 1, -1):
        font = fonts.get(size)
        lines = wrap(text, font, width)
        if max(font.getlength(line) for line in lines) <= width and len(lines) * int(size * 1.2) <= height:
            return size, lines
    return None


class Renderer:
    def __init__(self, spec: dict, i18n: dict, language: str):
        self.spec, self.i18n, self.language = spec, i18n, language
        self.fonts = Fonts(language)
        self.theme = spec["theme"]
        self.minimum = spec["display"]["min_font_px"]
        self.overflows: list[str] = []
        self.spec_actions = {a["id"]: a for a in spec["actions"]}

    def t(self, key: str) -> str:
        return self.i18n["keys"][key][self.language]

    # ---------------------------------------------------------------- what the signals read as in a state
    def values(self, state: model.State) -> dict[str, tuple[str, str]]:
        """signal id -> (text, tone) with tone in ok, warn, danger, muted, plain."""
        jetson_ok = state.link == 1
        link_text = {0: ("link.starting", "warn"), 1: ("link.ok", "ok"), 2: ("link.silent", "danger"), 3: ("link.spi_fault", "danger")}[state.link]
        bus_text = {0: ("link.ok", "ok"), 1: ("link.bus_warn", "warn"), 2: ("link.bus_off", "danger")}[state.bus]
        count = bin(state.alarms).count("1")
        level_key = "level." + model.ZONE_LEVELS[state.zone_level] if jetson_ok else "sys.not_confirmed"
        level_tone = "ok" if jetson_ok and state.zone_level == 0 else "muted" if not jetson_ok else "warn" if state.zone_level in (1, 3) else "danger"
        away = ("--", "muted")
        values = {
            "ui.mode": (self.t("mode." + model.MODES[state.mode]), {0: "warn", 1: "ok", 2: "warn", 3: "danger", 4: "danger"}[state.mode]),
            "ui.banner": (self.t("banner." + model.BANNERS[state.banner]), {0: "ok", 1: "warn", 2: "warn", 3: "warn", 4: "danger", 5: "danger", 6: "warn", 7: "danger", 8: "danger"}[state.banner]),
            "ui.listen_only": ("ON" if state.listen_only else "OFF", "warn" if state.listen_only else "plain"),
            "ui.jog_armed": (self.t("action.jog_armed") if state.jog_armed() else self.t("action.jog_not_armed"), "warn" if state.jog_armed() else "muted"),
            "ui.jog_arm_left_s": (str(max(0, (state.jog_armed_until - state.now + 999) // 1000)) if state.jog_armed() else "0", "plain"),
            "ui.alarm_count": (str(count), "danger" if count else "ok"),
            "ui.language": (self.t("settings.language_name"), "plain"),
            "ui.alarm_list": ("", "plain"),
            "estop.latched": (self.t("stop.latched") if state.latched or state.stop_pending else self.t("link.ok"), "danger" if state.latched or state.stop_pending else "ok"),
            "stack.can_state": (self.t(bus_text[0]), bus_text[1]),
            "stack.can_tx_errors": (str(state.can_tx), "warn" if state.can_tx >= state.cfg["can_warn_errors"] else "plain"),
            "stack.can_rx_errors": (str(state.can_rx), "warn" if state.can_rx >= state.cfg["can_warn_errors"] else "plain"),
            "robot.moving": ("1" if state.moving else "0", "warn" if state.moving else "plain"),
            "robot.x_mm": ("120.5", "plain"), "robot.y_mm": ("-48.0", "plain"), "robot.z_mm": ("15.2", "plain"),
            "mcu.temperature_c": ("41.5", "plain"), "mcu.uptime_s": ("128", "plain"),
            "jetson.link": (self.t(link_text[0]), link_text[1]),
            "jetson.age_ms": (str(max(0, state.now - state.last_heartbeat)) if state.seen_heartbeat else "--", "plain"),
            "jetson.spi_errors": (str(state.spi_errors), "plain"),
            "jetson.safety_level": (self.t(level_key), level_tone),
            "jetson.cpu_load_pct": ("23" if jetson_ok else away[0], "plain" if jetson_ok else "muted"),
            "jetson.temperature_c": ("52.0" if jetson_ok else away[0], "plain" if jetson_ok else "muted"),
            "jetson.camera_snapshot": ("", "plain"),
        }
        return values

    def colour(self, tone: str) -> tuple[str, str]:
        th = self.theme
        return {"ok": (th["ok"], th["ok_text"]), "warn": (th["warn"], th["warn_text"]), "danger": (th["danger"], th["danger_text"]), "muted": (th["disabled"], th["disabled_text"]),
                "plain": (th["surface"], th["text"])}[tone]

    # ---------------------------------------------------------------- drawing
    def text_in(self, draw: ImageDraw.ImageDraw, rect: dict, text: str, base: int, fill: str, where: str, anchor: str = "centre", pad: int = 8) -> None:
        width, height = rect["w"] - 2 * pad, rect["h"] - 4
        found = fit(text, self.fonts, width, height, base, self.minimum)
        if found is None:
            self.overflows.append(f"{where}: {text!r} does not fit {rect['w']}x{rect['h']}")
            found = (self.minimum, wrap(text, self.fonts.get(self.minimum), width))
        size, lines = found
        font = self.fonts.get(size)
        line_height = int(size * 1.2)
        top = rect["y"] + 2 + (height - len(lines) * line_height) // 2
        for index, line in enumerate(lines):
            length = font.getlength(line)
            left = rect["x"] + pad if anchor == "left" else rect["x"] + (rect["w"] - length) / 2
            draw.text((left, top + index * line_height), line, font=font, fill=fill)

    def draw_widget(self, draw: ImageDraw.ImageDraw, widget: dict, state: model.State, values: dict[str, tuple[str, str]], active_screen: str | None) -> None:
        th, rect, kind = self.theme, widget["rect"], widget["kind"]
        box = (rect["x"], rect["y"], rect["x"] + rect["w"] - 1, rect["y"] + rect["h"] - 1)
        where = widget["id"]
        label = self.t(widget["label_key"]) if "label_key" in widget else ""
        action = self.spec_actions.get(widget.get("action", ""))
        enabled = True
        if action is not None and action["kind"] != "nav":
            index = model.ACTIONS.index(action["id"]) if action["id"] in model.ACTIONS else -1
            enabled = index < 0 or state.refusal(index) is None
        if kind == "button":
            stop = widget.get("action") == "stop"
            fill = th["stop"] if stop else th["surface"] if enabled else th["disabled"]
            ink = th["stop_text"] if stop else th["text"] if enabled else th["disabled_text"]
            draw.rounded_rectangle(box, radius=12, fill=fill, outline=th["text_muted"] if not stop else th["stop_text"], width=3 if stop else 1)
            text = label + ("\n" + self.t("hint.hold_to_confirm") if widget.get("hold") and enabled else "")
            if widget.get("hold") and enabled:
                self.text_in(draw, {**rect, "h": rect["h"] * 2 // 3}, label, BASE_SIZE["button"], ink, where)
                self.text_in(draw, {**rect, "y": rect["y"] + rect["h"] * 2 // 3, "h": rect["h"] // 3}, self.t("hint.hold_to_confirm"), BASE_SIZE["label"], ink, where + "/hint", pad=2)
            else:
                self.text_in(draw, rect, label if not text.startswith("\n") else text, BASE_SIZE["button"] if not stop else 40, ink, where)
        elif kind == "tab":
            selected = widget["action"][4:] == active_screen
            draw.rectangle(box, fill=th["surface"] if selected else th["background"], outline=th["text_muted"])
            if selected:
                draw.rectangle((rect["x"], rect["y"] + rect["h"] - 6, rect["x"] + rect["w"] - 1, rect["y"] + rect["h"] - 1), fill=th["warn"])
            self.text_in(draw, rect, label, BASE_SIZE["tab"], th["text"] if selected else th["text_muted"], where)
        elif kind in ("value", "led"):
            text, tone = values[widget["signal"]]
            if "value_signal" in widget and widget["signal"] == "ui.jog_armed":
                text = text + "  " + values[widget["value_signal"]][0] + " " + self.t("unit.seconds")
            elif "unit_key" in widget and text != "--":
                text = text + " " + self.t(widget["unit_key"])
            fill, ink = self.colour(tone if kind == "led" or widget["id"].startswith("status_") else "plain")
            if kind == "value" and tone in ("danger", "warn", "ok") and (widget["signal"] in ("ui.mode", "jetson.safety_level", "ui.alarm_count") or widget["id"].startswith("status_")):
                fill, ink = self.colour(tone)
            draw.rounded_rectangle(box, radius=8, fill=fill, outline=th["text_muted"])
            small = rect["h"] < 80
            if small:
                self.text_in(draw, rect, f"{label}: {text}", BASE_SIZE["label"], ink, where, "left")
            else:
                self.text_in(draw, {**rect, "h": rect["h"] * 2 // 5, "y": rect["y"]}, label, BASE_SIZE["label"], ink, where + "/label", "left", pad=6)
                self.text_in(draw, {**rect, "y": rect["y"] + rect["h"] * 2 // 5, "h": rect["h"] * 3 // 5}, text, BASE_SIZE["value"], ink, where, "left", pad=10)
        elif kind == "toggle":
            text, tone = values[widget["signal"]]
            draw.rounded_rectangle(box, radius=8, fill=th["surface"], outline=th["text_muted"])
            on = text == "ON"
            self.text_in(draw, {**rect, "w": rect["w"] - 230, "h": rect["h"] * 2 // 3}, label, BASE_SIZE["button"], th["text"], where, "left", pad=12)
            if "hint_key" in widget:
                self.text_in(draw, {**rect, "y": rect["y"] + rect["h"] * 2 // 3, "w": rect["w"] - 230, "h": rect["h"] // 3}, self.t(widget["hint_key"]), BASE_SIZE["label"], th["text_muted"], where + "/hint", "left", pad=2)
            knob = (rect["x"] + rect["w"] - 130, rect["y"] + rect["h"] // 2 - 22, rect["x"] + rect["w"] - 20, rect["y"] + rect["h"] // 2 + 22)
            draw.rounded_rectangle(knob, radius=22, fill=th["warn"] if on else th["disabled"])
            cx = knob[2] - 24 if on else knob[0] + 24
            draw.ellipse((cx - 18, knob[1] + 4, cx + 18, knob[3] - 4), fill=th["text"])
            self.text_in(draw, {"x": knob[0] - 70, "y": knob[1] + 6, "w": 64, "h": 32}, text, BASE_SIZE["label"], th["text"], where + "/state", pad=0)
        elif kind == "banner":
            text, tone = values[widget["signal"]]
            fill, ink = self.colour(tone)
            draw.rounded_rectangle(box, radius=10, fill=fill, outline=th["text_muted"])
            self.text_in(draw, rect, text, BASE_SIZE["banner"], ink, where, "left", pad=14)
        elif kind == "list":
            draw.rounded_rectangle(box, radius=8, fill=th["surface"], outline=th["text_muted"])
            names = [(n, model.ALARMS[n]) for n in range(len(model.ALARMS)) if state.alarms & (1 << n)]
            names.sort(key=lambda item: -item[1][1])
            if not names:
                self.text_in(draw, rect, label, BASE_SIZE["value"], th["text_muted"], where)
            for row, (number, (name, level)) in enumerate(names[:7]):
                top = rect["y"] + 8 + row * 60
                tone = {1: "warn", 2: "danger", 3: "danger"}[level]
                fill, ink = self.colour(tone)
                tag = {1: "!", 2: "!!", 3: "X"}[level]
                draw.rounded_rectangle((rect["x"] + 8, top, rect["x"] + rect["w"] - 9, top + 52), radius=6, fill=fill)
                acked = bool(state.acked & (1 << number))
                text = f"{tag}  {self.t('alarm.' + name)}" + ("  (OK)" if acked else "")
                self.text_in(draw, {"x": rect["x"] + 8, "y": top, "w": rect["w"] - 16, "h": 52}, text, BASE_SIZE["button"], ink, where + f"/row{row}", "left", pad=10)
        elif kind == "image":
            draw.rectangle(box, fill=th["disabled"], outline=th["text_muted"])
            draw.line((box[0], box[1], box[2], box[3]), fill=th["text_muted"], width=2)
            draw.line((box[0], box[3], box[2], box[1]), fill=th["text_muted"], width=2)
        elif kind == "text":
            self.text_in(draw, rect, label, BASE_SIZE["note"], th["text"], where, "left", pad=6)

    def render(self, state: model.State, screen_id: str) -> Image.Image:
        spec = self.spec
        image = Image.new("RGB", (spec["display"]["width"], spec["display"]["height"]), self.theme["background"])
        draw = ImageDraw.Draw(image)
        values = self.values(state)
        screen = next(sc for sc in spec["screens"] if sc["id"] == screen_id)
        for widget in spec["global_widgets"]:
            self.draw_widget(draw, widget, state, values, screen_id)
        needs_away = screen["needs_jetson"] and state.link != 1
        for widget in screen["widgets"]:
            if needs_away and widget["kind"] == "image":
                draw.rectangle((widget["rect"]["x"], widget["rect"]["y"], widget["rect"]["x"] + widget["rect"]["w"] - 1, widget["rect"]["y"] + widget["rect"]["h"] - 1), fill=self.theme["disabled"])
                continue
            if needs_away and widget["kind"] == "text":
                self.draw_widget(draw, widget, state, values, screen_id)
                continue
            self.draw_widget(draw, widget, state, values, screen_id)
        return image


def scene_state(scene: str) -> model.State:
    """A state of the view-model reached through events, as the firmware would reach it."""
    state = model.State(model.load_config())
    state._derive()
    state.event(0, "HB", 0)
    if scene == "normal":
        state.event(100, "HB", 0)
        state.event(200, "DOWN", model.A_JOG_ARM)
        state.event(300, "HB", 0)
    elif scene == "degraded":
        state.event(100, "HB", 0)
        state.event(3000, "TICK")
    elif scene == "stopped":
        state.event(100, "DOWN", model.A_STOP)
        state.event(150, "ESTOP", 1)
        state.event(200, "HB", 0)
    elif scene == "alarm":
        state.event(100, "HB", 2)
        state.event(200, "CAN", 0, 120, 30)
    elif scene == "listen_only":
        state.event(100, "DOWN", model.A_LISTEN)
    else:
        raise ValueError(scene)
    return state


def render_set(folder: Path, languages: tuple[str, ...], scenes: tuple[str, ...], screens: tuple[str, ...]) -> list[Path]:
    spec, i18n = ui_spec.load_spec(), ui_spec.load_i18n()
    written: list[Path] = []
    problems: list[str] = []
    folder.mkdir(parents=True, exist_ok=True)
    for language in languages:
        renderer = Renderer(spec, i18n, language)
        for scene in scenes:
            for screen in screens:
                image = renderer.render(scene_state(scene), screen)
                path = folder / f"ui_{scene}_{screen}_{language}.png"
                image.save(path, optimize=True)
                written.append(path)
        problems += [f"{language}: {line}" for line in sorted(set(renderer.overflows))]
    if problems:
        raise SystemExit(f"PROBLEM ({len(problems)}): " + "; ".join(problems[:40]))
    return written


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("folder")
    parser.add_argument("--scene", default="normal", choices=SCENES)
    parser.add_argument("--language", default="en", choices=ui_spec.LANGUAGES)
    parser.add_argument("--screen", default="dashboard", choices=model.SCREENS)
    parser.add_argument("--all", action="store_true", help="every language, every scene, every screen")
    options = parser.parse_args(argv)
    if options.all:
        paths = render_set(Path(options.folder), ui_spec.LANGUAGES, SCENES, model.SCREENS)
    else:
        paths = render_set(Path(options.folder), (options.language,), (options.scene,), (options.screen,))
    for path in paths:
        print("wrote", path)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
