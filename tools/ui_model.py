#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/ui_model.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Reference of the view-model of the interface of the STM32H757 (src/ui_h757/core/ui_core.c is its twin in C, tests/test_ui_core.py proves they agree on every scenario).

The model decides what the panel shows and which touches it believes: the mode (starting, normal, degraded, alarm, stopped), the state of the link to the Jetson and of
the stack bus, the alarms, the banner, and for every action whether it is allowed and, if not, why. It has no clock, no display and no bus: an event carries the time, and the
answer of an event is a list of effects (what the firmware must do: send the stop, ask for a release, a jog step, remember a setting). That makes it deterministic, so the
scenarios of src/ui_h757/spec/scenarios.json (the Jetson hangs, the SPI link is cut, the bus fills with errors, STOP while moving, listen-only) replay byte for byte.

The rules that never change: STOP is allowed in every mode and is sent on the first touch; nothing but STOP and local settings works while listen-only is on; with the
Jetson not answering nothing moves and the stop is not released unless a switch of config/ui.cfg says so; nothing the panel cannot confirm is shown as good.
Evidence: simulator (host tests); nothing here has run on a display or a board."""
from __future__ import annotations

import configparser
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CFG_PATH = ROOT / "config" / "ui.cfg"

MODES = ("boot", "normal", "degraded", "alarm", "stopped")
LINKS = ("starting", "ok", "silent", "spi_fault")
BUSES = ("ok", "warn", "off")
BANNERS = ("normal", "boot", "degraded", "spi_fault", "bus_off", "stopped", "listen_only", "alarm", "stop_unconfirmed", "locked")
SCREENS = ("dashboard", "control", "camera", "position", "metrics", "system", "alarms", "settings", "unlock")
ZONE_LEVELS = ("ready", "warning", "danger", "inhibited", "fault", "unknown")
ZONE_UNKNOWN = 5
REFUSALS = ("listen_only", "not_stopped", "stopped", "degraded", "bus_fault", "not_armed", "alarm", "starting", "locked")
ACTIONS = ("stop", "release", "jog_arm", "jog_x_plus", "jog_x_minus", "jog_y_plus", "jog_y_minus", "jog_z_plus", "jog_z_minus", "ack_alarm", "toggle_listen_only",
           "next_language")
A_STOP, A_RELEASE, A_JOG_ARM, A_JOG_FIRST, A_JOG_LAST, A_ACK, A_LISTEN, A_LANG = 0, 1, 2, 3, 8, 9, 10, 11
HOLD_ACTIONS = (A_RELEASE, A_LISTEN)

# alarm number -> (name, level) with level 1 warning, 2 danger, 3 fault; the text key of each is alarm.<name>
ALARMS = (("jetson_lost", 1), ("spi_link", 1), ("can_errors", 1), ("can_bus_off", 3), ("stop_unconfirmed", 3), ("motion_after_stop", 3), ("zone_warning", 1),
          ("zone_danger", 2), ("zone_inhibited", 2), ("zone_fault", 3))
(AL_JETSON, AL_SPI, AL_CAN_ERR, AL_CAN_OFF, AL_STOP_UNCONF, AL_MOTION, AL_ZONE_WARN, AL_ZONE_DANGER, AL_ZONE_INHIBIT, AL_ZONE_FAULT) = range(10)

CONFIG_FIELDS = ("jetson_timeout_ms", "jetson_boot_grace_ms", "spi_fault_after_errors", "stop_confirm_ms", "stop_motion_ms", "release_hold_ms", "listen_off_hold_ms",
                 "jog_arm_ms", "can_warn_errors", "degraded_allow_release", "degraded_allow_motion", "operator_pin_enabled", "pin_max_failures", "pin_lockout_ms", "pin_idle_ms")
CONFIG_LIMITS = {
    "jetson_timeout_ms": (200, 60000), "jetson_boot_grace_ms": (0, 600000), "spi_fault_after_errors": (1, 1000), "stop_confirm_ms": (50, 10000),
    "stop_motion_ms": (50, 60000), "release_hold_ms": (500, 5000), "listen_off_hold_ms": (500, 5000), "jog_arm_ms": (1000, 60000),
    "can_warn_errors": (1, 255), "degraded_allow_release": (0, 1), "degraded_allow_motion": (0, 1), "operator_pin_enabled": (0, 1), "pin_max_failures": (1, 10),
    "pin_lockout_ms": (1000, 600000), "pin_idle_ms": (5000, 3600000),
}


class ConfigError(ValueError):
    pass


def load_config(path: Path = CFG_PATH) -> dict[str, int]:
    parser = configparser.ConfigParser(inline_comment_prefixes=None)
    try:
        parser.read_string(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, configparser.Error) as error:
        raise ConfigError(f"cannot read {path.name}: {error.__class__.__name__}") from error
    if "ui" not in parser:
        raise ConfigError("section [ui] is missing")
    section = parser["ui"]
    unknown = set(section) - set(CONFIG_FIELDS)
    if unknown:
        raise ConfigError(f"unknown keys {sorted(unknown)}")
    values: dict[str, int] = {}
    for name in CONFIG_FIELDS:
        raw = section.get(name, "").strip()
        try:
            value = int(raw, 10)
        except ValueError as error:
            raise ConfigError(f"{name} must be a whole number, not {raw!r}") from error
        low, high = CONFIG_LIMITS[name]
        if not low <= value <= high:
            raise ConfigError(f"{name} must be {low}..{high}, not {value}")
        values[name] = value
    return values


def core_text_keys() -> set[str]:
    """The text keys the view-model and the renderer pick by themselves (the others are named by a widget of src/ui_h757/spec/ui_spec.json)."""
    keys = {f"mode.{name}" for name in MODES} | {f"banner.{name}" for name in BANNERS} | {f"refused.{name}" for name in REFUSALS}
    keys |= {f"alarm.{name}" for name, _ in ALARMS} | {f"level.{name}" for name in ZONE_LEVELS}
    keys |= {"link.ok", "link.starting", "link.silent", "link.spi_fault", "link.bus_warn", "link.bus_off", "stop.sent", "stop.latched", "alarm.none",
             "hint.hold_to_confirm", "action.jog_armed", "action.jog_not_armed", "settings.language_name", "sys.not_confirmed", "cam.needs_jetson",
             "lock.wrong_pin", "lock.locked_out", "lock.state_locked", "lock.state_open", "lock.state_off"}
    return keys


@dataclass
class State:
    cfg: dict[str, int]
    now: int = 0
    seen_heartbeat: bool = False
    last_heartbeat: int = 0
    zone_level: int = ZONE_UNKNOWN
    spi_error_run: int = 0
    spi_errors: int = 0
    can_off: bool = False
    can_tx: int = 0
    can_rx: int = 0
    latched: bool = False
    latched_since: int = 0
    stop_pending: bool = False
    stop_sent: int = 0
    moving: bool = False
    listen_only: bool = False
    jog_armed_until: int = 0  # 0 = not armed
    screen: int = 0
    language: int = 0
    hold_action: int = -1
    hold_start: int = 0
    acked: int = 0
    pin_unlocked: bool = False
    pin_failures: int = 0
    pin_lockout_until: int = 0
    last_touch: int = 0
    # derived after every event
    link: int = 0
    bus: int = 0
    alarms: int = 0
    mode: int = 0
    banner: int = 1
    caps: int = 0
    effects: list[str] = field(default_factory=list)

    # ------------------------------------------------------------ derived values
    def locked(self) -> bool:
        """The operator PIN is on and has not been given (or has expired). STOP never asks for it."""
        return bool(self.cfg["operator_pin_enabled"]) and not self.pin_unlocked

    def jog_armed(self) -> bool:
        return self.jog_armed_until != 0 and self.now < self.jog_armed_until

    def _link(self) -> int:
        fault = self.spi_error_run >= self.cfg["spi_fault_after_errors"]
        if self.seen_heartbeat and self.now - self.last_heartbeat <= self.cfg["jetson_timeout_ms"]:
            return 1
        if not self.seen_heartbeat and self.now < self.cfg["jetson_boot_grace_ms"] and not fault:
            return 0
        return 3 if fault else 2

    def _bus(self) -> int:
        if self.can_off:
            return 2
        return 1 if max(self.can_tx, self.can_rx) >= self.cfg["can_warn_errors"] else 0

    def _alarm_mask(self) -> int:
        mask = 0
        if self.link == 2:
            mask |= 1 << AL_JETSON
        if self.link == 3:
            mask |= 1 << AL_SPI
        if self.bus == 1:
            mask |= 1 << AL_CAN_ERR
        if self.bus == 2:
            mask |= 1 << AL_CAN_OFF
        if self.stop_pending and self.now - self.stop_sent >= self.cfg["stop_confirm_ms"]:
            mask |= 1 << AL_STOP_UNCONF
        if self.latched and self.moving and self.now - self.latched_since >= self.cfg["stop_motion_ms"]:
            mask |= 1 << AL_MOTION
        if self.link == 1:
            mask |= {1: 1 << AL_ZONE_WARN, 2: 1 << AL_ZONE_DANGER, 3: 1 << AL_ZONE_INHIBIT, 4: 1 << AL_ZONE_FAULT}.get(self.zone_level, 0)
        return mask

    def worst_level(self) -> int:
        worst = 0
        for number, (_, level) in enumerate(ALARMS):
            if self.alarms & (1 << number):
                worst = max(worst, level)
        return worst

    def _mode(self) -> int:
        if self.latched or self.stop_pending:
            return 4
        if self.worst_level() >= 2:
            return 3
        if self.link in (2, 3):
            return 2
        if self.link == 0:
            return 0
        return 1

    def _banner(self) -> int:
        if self.alarms & (1 << AL_STOP_UNCONF):
            return 8
        if self.mode == 4:
            return 5
        if self.bus == 2:
            return 4
        if self.mode == 3:
            return 7
        if self.link == 2:
            return 2
        if self.link == 3:
            return 3
        if self.link == 0:
            return 1
        if self.locked():
            return 9
        if self.listen_only:
            return 6
        return 0

    # ------------------------------------------------------------ what may be done
    def _motion_refusal(self) -> int | None:
        """Why nothing may move now, or None."""
        if self.listen_only:
            return 0
        if self.locked():
            return 8
        if self.mode == 4:
            return 2
        if self.mode == 0:
            return 7
        if self.bus == 2:
            return 4
        if self.link in (2, 3) and not self.cfg["degraded_allow_motion"]:
            return 3
        if self.mode == 3:
            return 6
        return None

    def refusal(self, action: int) -> int | None:
        """Why the action is refused now (an index of REFUSALS), or None when it is allowed. STOP, the acknowledge, the language and listen-only are never refused."""
        if action == A_RELEASE:
            if self.listen_only:
                return 0
            if self.locked():
                return 8
            if self.bus == 2:
                return 4
            if not self.latched:
                return 1
            if self.link != 1 and not self.cfg["degraded_allow_release"]:
                return 7 if self.link == 0 else 3
            if self.alarms & ((1 << AL_ZONE_DANGER) | (1 << AL_ZONE_INHIBIT) | (1 << AL_ZONE_FAULT)):
                return 6
            return None
        if action == A_JOG_ARM:
            return self._motion_refusal()
        if A_JOG_ARM < action <= A_JOG_LAST:
            blocked = self._motion_refusal()
            if blocked is not None:
                return blocked
            return None if self.jog_armed() else 5
        return None

    def _derive(self) -> None:
        before = self.alarms
        if self.cfg["operator_pin_enabled"] and self.pin_unlocked and self.now - self.last_touch >= self.cfg["pin_idle_ms"]:
            self.pin_unlocked = False
            self.effects.append("locked")
        self.link = self._link()
        self.bus = self._bus()
        self.alarms = self._alarm_mask()
        self.mode = self._mode()
        if self.jog_armed_until != 0 and (not self.jog_armed() or self._motion_refusal() is not None):
            self.jog_armed_until = 0
        self.acked &= self.alarms
        self.banner = self._banner()
        caps = 0
        for action in range(len(ACTIONS)):
            if self.refusal(action) is None:
                caps |= 1 << action
        self.caps = caps
        for number in range(len(ALARMS)):
            bit = 1 << number
            if (self.alarms & bit) and not (before & bit):
                self.effects.append(f"alarm+:{ALARMS[number][0]}")
            elif (before & bit) and not (self.alarms & bit):
                self.effects.append(f"alarm-:{ALARMS[number][0]}")
        if self.hold_action >= 0 and self.refusal(self.hold_action) is not None and not (self.hold_action == A_LISTEN):
            reason = REFUSALS[self.refusal(self.hold_action) or 0]
            self.effects.append(f"refused:{ACTIONS[self.hold_action]}:{reason}")
            self.hold_action = -1

    # ------------------------------------------------------------ events
    def event(self, now: int, name: str, *args: int) -> list[str]:
        """Feeds one event. Returns the effects. Time never goes back."""
        self.effects = []
        self.now = max(self.now, now)
        if name == "HB":
            self.seen_heartbeat = True
            self.last_heartbeat = self.now
            self.zone_level = args[0] if args and 0 <= args[0] <= ZONE_UNKNOWN else ZONE_UNKNOWN
            self.spi_error_run = 0
        elif name == "SPI_ERR":
            self.spi_error_run += 1
            self.spi_errors += 1
        elif name == "SPI_OK":
            self.spi_error_run = 0
        elif name == "CAN":
            self.can_off = bool(args[0])
            self.can_tx = max(0, min(255, args[1]))
            self.can_rx = max(0, min(255, args[2]))
        elif name == "ESTOP":
            if args[0] and not self.latched:
                self.latched_since = self.now
            self.latched = bool(args[0])
            self.stop_pending = False
        elif name == "MOTION":
            self.moving = bool(args[0])
        elif name == "NAV":
            if 0 <= args[0] < len(SCREENS):
                self.screen = args[0]
        elif name == "PIN_OK":
            self._pin_ok()
        elif name == "PIN_BAD":
            self._pin_bad()
        elif name == "DOWN":
            self._derive()
            self.last_touch = self.now  # a touch is judged against the state of this moment, not of the last event
            self._down(args[0])
        elif name == "UP":
            self.last_touch = self.now
            if 0 <= args[0] < len(ACTIONS) and self.hold_action == args[0]:
                self.hold_action = -1
                self.effects.append("hold_cancel")
        elif name != "TICK":
            raise ValueError(f"unknown event {name}")
        self._derive()
        self._hold_tick()
        return self.effects

    def _hold_tick(self) -> None:
        if self.hold_action < 0:
            return
        needed = self.cfg["release_hold_ms"] if self.hold_action == A_RELEASE else self.cfg["listen_off_hold_ms"]
        if self.now - self.hold_start < needed:
            return
        action = self.hold_action
        self.hold_action = -1
        if action == A_RELEASE:
            reason = self.refusal(A_RELEASE)
            if reason is None:
                self.effects.append("release")
            else:
                self.effects.append(f"refused:release:{REFUSALS[reason]}")
        else:
            self.listen_only = False
            self.effects.append("listen:0")
            self._derive()

    def _down(self, action: int) -> None:
        if not 0 <= action < len(ACTIONS):
            return
        if action == A_STOP:
            self.effects.append("stop")
            self.jog_armed_until = 0
            if not self.latched and not self.stop_pending:
                self.stop_pending = True
                self.stop_sent = self.now
            return
        if action == A_ACK:
            self.acked = self.alarms
            self.effects.append("ack")
            return
        if action == A_LANG:
            self.language = (self.language + 1) % 8
            self.effects.append(f"lang:{self.language}")
            return
        if action == A_LISTEN:
            if self.listen_only and self.locked():
                self.effects.append("refused:toggle_listen_only:locked")
            elif self.listen_only:
                self.hold_action, self.hold_start = A_LISTEN, self.now
            else:
                self.listen_only = True
                self.jog_armed_until = 0
                self.hold_action = -1
                self.effects.append("listen:1")
            return
        reason = self.refusal(action)
        if reason is not None:
            self.effects.append(f"refused:{ACTIONS[action]}:{REFUSALS[reason]}")
            return
        if action == A_RELEASE:
            self.hold_action, self.hold_start = A_RELEASE, self.now
        elif action == A_JOG_ARM:
            self.jog_armed_until = self.now + self.cfg["jog_arm_ms"]
            self.effects.append("jog_armed")
        else:
            self.effects.append("jog:" + ACTIONS[action][4:])

    def _pin_ok(self) -> None:
        """The port says the PIN typed was right. Ignored while a lock-out lasts or when the PIN is off."""
        if not self.cfg["operator_pin_enabled"] or self.now < self.pin_lockout_until:
            return
        self.pin_failures = 0
        self.last_touch = self.now
        if not self.pin_unlocked:
            self.pin_unlocked = True
            self.effects.append("unlocked")

    def _pin_bad(self) -> None:
        """The port says the PIN typed was wrong: after pin_max_failures in a row the pad is locked out for pin_lockout_ms."""
        if not self.cfg["operator_pin_enabled"] or self.now < self.pin_lockout_until:
            return
        self.pin_failures += 1
        if self.pin_failures >= self.cfg["pin_max_failures"]:
            self.pin_failures = 0
            self.pin_lockout_until = self.now + self.cfg["pin_lockout_ms"]
            self.effects.append("pin_lockout")
        else:
            self.effects.append("pin_wrong")

    # ------------------------------------------------------------ the line of the trace (the twin in C prints the same)
    def trace(self, now: int, name: str) -> str:
        hold = ACTIONS[self.hold_action] if self.hold_action >= 0 else "-"
        unacked = self.alarms & ~self.acked
        effects = ",".join(self.effects) if self.effects else "-"
        return (f"t={now} ev={name} mode={MODES[self.mode]} link={LINKS[self.link]} bus={BUSES[self.bus]} lo={int(self.listen_only)} lk={int(self.locked())} scr={self.screen} "
                f"al={self.alarms:03x} un={unacked:03x} arm={int(self.jog_armed())} stop={int(self.latched)}{int(self.stop_pending)} hold={hold} "
                f"ban={BANNERS[self.banner]} caps={self.caps:04x} fx={effects}")


def parse_event(line: str) -> tuple[int, str, tuple[int, ...]]:
    """`1500 HB 0` -> (1500, "HB", (0,)); the arguments of DOWN/UP/NAV may be names of an action or of a screen."""
    parts = line.split()
    now, name, rest = int(parts[0]), parts[1], parts[2:]
    numbers: list[int] = []
    for token in rest:
        if name in ("DOWN", "UP") and token in ACTIONS:
            numbers.append(ACTIONS.index(token))
        elif name == "NAV" and token in SCREENS:
            numbers.append(SCREENS.index(token))
        else:
            numbers.append(int(token))
    return now, name, tuple(numbers)


def run_scenario(scenario: dict, base: dict[str, int]) -> list[str]:
    cfg = dict(base)
    cfg.update(scenario.get("cfg", {}))
    state = State(cfg)
    state._derive()
    lines = []
    for text in scenario["events"]:
        now, name, args = parse_event(text)
        state.event(now, name, *args)
        lines.append(state.trace(state.now, name))
    return lines
