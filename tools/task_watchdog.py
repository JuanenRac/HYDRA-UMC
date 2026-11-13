#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/task_watchdog.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Reference of the supervision of liveness by task: a hung control loop must lead to the outputs being cut before the time the safety file requires. The C twin is
src/mcu_stm32h757/CM4/task_watchdog.h.

Each task checks in each time it finishes a pass. The supervisor looks at the check-ins every `update_period_ms` and hands the hardware watchdog its refresh ONLY while no mandatory task
is late. A mandatory task that goes without checking in for longer than its deadline is a hang: the supervisor asks for the stop once and latches (it never refreshes again, so the
hardware watchdog cuts the outputs even if the hung task wakes up and checks in). A task that is not mandatory only marks the system degraded and recovers when it checks in again.
The worst time from a hang to the cut is `bound_ms`: the longest mandatory deadline, plus one supervisor period, plus the timeout of the hardware watchdog; the configuration is refused
when that is longer than `required_cut_ms`. The times are in config/task_watchdog.cfg. Evidence: simulator (host tests of the reference and of the C twin against the same vectors);
nothing here ran on a board and the real watchdog was not frozen."""
from __future__ import annotations

import configparser
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CFG_PATH = ROOT / "config" / "task_watchdog.cfg"
MAX_TASKS = 8

ST_RUN, ST_DEGRADED, ST_TRIPPED = 0, 1, 2
STATE_NAMES = ("run", "degraded", "tripped")


def bound_ms(tasks: list[tuple[str, int, int]], update_period_ms: int, iwdg_timeout_ms: int) -> int:
    longest = max((deadline for _, deadline, mandatory in tasks if mandatory), default=0)
    return longest + update_period_ms + iwdg_timeout_ms


def load_config(path: Path = CFG_PATH) -> dict:
    parser = configparser.ConfigParser(inline_comment_prefixes=None)
    parser.read_string(path.read_text(encoding="utf-8"))
    section = parser["watchdog"]
    cfg: dict = {name: int(section[name]) for name in ("update_period_ms", "iwdg_timeout_ms", "required_cut_ms")}
    if not 1 <= cfg["update_period_ms"] <= 100 or not 10 <= cfg["iwdg_timeout_ms"] <= 5000:
        raise ValueError("config/task_watchdog.cfg: update_period_ms 1..100 and iwdg_timeout_ms 10..5000")
    tasks = []
    for name in parser.sections():
        if not name.startswith("task."):
            continue
        deadline, mandatory = int(parser[name]["deadline_ms"]), int(parser[name]["mandatory"])
        if not 1 <= deadline <= 2000 or mandatory not in (0, 1):
            raise ValueError(f"config/task_watchdog.cfg: [{name}] needs deadline_ms 1..2000 and mandatory 0 or 1")
        tasks.append((name[5:], deadline, mandatory))
    if not 1 <= len(tasks) <= MAX_TASKS or not any(m for _, _, m in tasks):
        raise ValueError(f"config/task_watchdog.cfg: 1..{MAX_TASKS} tasks and at least one mandatory")
    cfg["tasks"] = tasks
    cfg["bound_ms"] = bound_ms(tasks, cfg["update_period_ms"], cfg["iwdg_timeout_ms"])
    if cfg["bound_ms"] > cfg["required_cut_ms"]:
        raise ValueError(f"config/task_watchdog.cfg: the worst time to the cut is {cfg['bound_ms']} ms, over the {cfg['required_cut_ms']} ms required")
    return cfg


@dataclass
class TaskWatchdog:
    deadlines: list[int]
    mandatory: list[int]
    started: int = 0
    last: list[int] = field(default_factory=list)
    state: int = ST_RUN
    late_mask: int = 0
    trips: int = 0

    def start(self, now_ms: int) -> None:
        self.started = now_ms
        self.last = [now_ms] * len(self.deadlines)
        self.state, self.late_mask, self.trips = ST_RUN, 0, 0

    def check_in(self, task: int, now_ms: int) -> None:
        if 0 <= task < len(self.deadlines):  # a number that is no task is ignored, never trusted as a sign of life
            self.last[task] = now_ms

    def update(self, now_ms: int) -> bool:
        """Looks at the check-ins. Returns True when the hardware watchdog may be refreshed after this update. `trips` grows once, the moment a mandatory task is first late."""
        late = 0
        for i, deadline in enumerate(self.deadlines):
            if ((now_ms - self.last[i]) & 0xFFFFFFFF) > deadline:
                late |= 1 << i
        mandatory_late = any(late >> i & 1 and self.mandatory[i] for i in range(len(self.deadlines)))
        if self.state != ST_TRIPPED:
            if mandatory_late:
                self.state = ST_TRIPPED
                self.trips += 1
            else:
                self.state = ST_DEGRADED if late else ST_RUN
            self.late_mask = late
        return self.state != ST_TRIPPED


def from_config(cfg: dict) -> TaskWatchdog:
    return TaskWatchdog([d for _, d, _ in cfg["tasks"]], [m for _, _, m in cfg["tasks"]])
