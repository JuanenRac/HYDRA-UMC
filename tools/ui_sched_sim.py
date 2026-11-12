#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/ui_sched_sim.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""A model of the task priorities of the interface of the STM32H757: how long from a touch of STOP to the stop frame on the stack bus, whatever the drawing and the PIN check do.

    python tools/ui_sched_sim.py          print the worst latency for a light and a heavy interface and exit 1 when it is above the limit of config/ui.cfg

The model is a fixed-priority preemptive scheduler in steps of 10 microseconds: the touch interrupt (it only leaves a request), the task of the stack bus (period 1 ms; when it runs and finds
a request it sends the stop frame), the task of the Jetson link, the interface task that draws, and the PIN check. The numbers are in `config/ui.cfg` ([sched]) and are ESTIMATES of
the design, not measurements: this is the argument that the stop path does not wait for drawing when the priorities are set as designed (the stack task above the interface task, the
touch interrupt above both), and a counter-model that shows the argument fails when they are not. It is NOT FreeRTOS: the real latency is measured on the board (HY0253).
Evidence: simulator (a model of the design)."""
from __future__ import annotations

import configparser
import random
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
STEP_US = 10
FIELDS = ("stack_task_period_us", "stack_task_wcet_us", "touch_isr_us", "link_task_period_ms", "link_task_wcet_us", "ui_task_period_ms", "ui_task_wcet_ms", "pin_check_ms", "stop_max_latency_ms")


def load(path: Path = ROOT / "config" / "ui.cfg") -> dict[str, int]:
    parser = configparser.ConfigParser(inline_comment_prefixes=None)
    parser.read_string(path.read_text(encoding="utf-8"))
    section = parser["sched"]
    return {name: int(section[name]) for name in FIELDS}


@dataclass
class Task:
    name: str
    priority: int  # higher runs first
    period_us: int
    wcet_us: int
    remaining: int = 0
    next_release: int = 0
    one_shot: bool = False


def worst_latency_us(settings: dict[str, int], ui_priority: int = 1, stack_priority: int = 3, ui_wcet_ms: int | None = None, with_pin_check: bool = True, seconds: float = 2.0,
                     touches: int = 300, seed: int = 7) -> int:
    """The longest time from a touch of STOP to the stop frame, over `touches` touches at random moments (fixed seed)."""
    ui_wcet = (settings["ui_task_wcet_ms"] if ui_wcet_ms is None else ui_wcet_ms) * 1000
    tasks = [
        Task("stack", stack_priority, settings["stack_task_period_us"], settings["stack_task_wcet_us"]),
        Task("link", 2, settings["link_task_period_ms"] * 1000, settings["link_task_wcet_us"]),
        Task("ui", ui_priority, settings["ui_task_period_ms"] * 1000, ui_wcet),
    ]
    pin = Task("pin", ui_priority, 10**12, settings["pin_check_ms"] * 1000, one_shot=True)
    if with_pin_check:
        tasks.append(pin)
    rng = random.Random(seed)
    end = int(seconds * 1_000_000)
    touch_times = sorted(rng.randrange(0, end - 400_000) for _ in range(touches))
    pending: list[int] = []  # touches whose request the stack task has not yet seen
    isr_left = 0
    isr_for: list[int] = []
    worst = 0
    time = 0
    while time < end:
        # the touch interrupt preempts everything: it takes touch_isr_us and then leaves the request
        while touch_times and touch_times[0] <= time:
            isr_for.append(touch_times.pop(0))
            isr_left += settings["touch_isr_us"]
        for t in tasks:
            if time >= t.next_release and t.remaining == 0:
                t.remaining = t.wcet_us
                t.next_release = time + t.period_us
        if isr_left > 0:
            isr_left -= STEP_US
            if isr_left <= 0:
                isr_left = 0
                pending.extend(isr_for)
                isr_for = []
        else:
            ready = [t for t in tasks if t.remaining > 0]
            if ready:
                running = max(ready, key=lambda t: t.priority)
                running.remaining -= STEP_US
                if running.name == "stack" and running.remaining <= 0:
                    running.remaining = 0
                    for touched in pending:
                        worst = max(worst, time + STEP_US - touched)
                    pending = []
        time += STEP_US
    for touched in pending + isr_for:
        worst = max(worst, end - touched)  # a stop that was never sent counts as the whole rest of the run
    return worst


def estimate() -> dict:
    s = load()
    light = worst_latency_us(s, ui_wcet_ms=1, with_pin_check=False)
    configured = worst_latency_us(s)
    heavy = worst_latency_us(s, ui_wcet_ms=300)
    wrong = worst_latency_us(s, ui_priority=4, ui_wcet_ms=300)
    limit = s["stop_max_latency_ms"] * 1000
    return {"light_us": light, "configured_us": configured, "heavy_us": heavy, "ui_above_stack_us": wrong, "limit_us": limit, "fits": max(light, configured, heavy) <= limit}


def main() -> int:
    n = estimate()
    print(f"touch of STOP to the stop frame: {n['light_us']} us with a light interface, {n['configured_us']} us as configured, {n['heavy_us']} us with a 300 ms frame and the PIN check running "
          f"(limit {n['limit_us']} us: {'fits' if n['fits'] else 'DOES NOT FIT'})")
    print(f"counter-model with the interface task above the stack task: {n['ui_above_stack_us']} us")
    print("ui_sched_sim:", "PASS" if n["fits"] else "FAIL")
    return 0 if n["fits"] else 1


if __name__ == "__main__":
    sys.exit(main())
