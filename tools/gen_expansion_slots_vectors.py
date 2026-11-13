#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/gen_expansion_slots_vectors.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Writes tests/expansion_slots_vectors.h: scenarios of the power sequence of the expansion slots run through the Python reference (tools/expansion_slots.py), so that the C twin
(src/mcu_stm32h757/Common/expansion_slots.h) is held to the same states.

    python tools/gen_expansion_slots_vectors.py           write the header
    python tools/gen_expansion_slots_vectors.py --check   exit 1 when the header is not what the reference says

A step is one update at a time with the masks of the cards present and of the fuses in fault, and optionally a command of a person (clear a fault, unlock a slot) before it. After the step
the header holds the state of the three slots, the enable and reset-released masks and the fault count of each slot. Evidence: simulator."""
from __future__ import annotations

import random
import sys
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import expansion_slots as ex  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
HEADER_PATH = ROOT / "tests" / "expansion_slots_vectors.h"

NONE, CLEAR, UNLOCK = 0, 1, 2  # the command kind; the slot index goes next to it


@dataclass(frozen=True)
class Step:
    t: int
    present: int = 0
    fault: int = 0
    cmd: int = NONE
    slot: int = 0


def config(name: str) -> dict:
    base = ex.load_config()
    return {
        "file_default_all_off": dict(base),
        "slots_1_and_3": dict(base, allowed_mask=0b101),
        "all_allowed": dict(base, allowed_mask=0b111),
        "locks_after_two": dict(base, allowed_mask=0b111, max_faults=2, fault_debounce_ms=0),
    }[name]


CONFIG_NAMES = ["file_default_all_off", "slots_1_and_3", "all_allowed", "locks_after_two"]


def run(cfg_name: str, steps: list[Step]) -> list[tuple]:
    model = ex.ExpansionSlots(config(cfg_name))
    model.start(steps[0].t)
    rows = []
    for step in steps:
        if step.cmd == CLEAR:
            model.clear_fault(step.slot)
        elif step.cmd == UNLOCK:
            model.unlock(step.slot, step.t)
        model.update(step.t, step.present, step.fault)
        s = model.slots
        rows.append(
            (
                s[0].state,
                s[1].state,
                s[2].state,
                sum(int(x.enable) << i for i, x in enumerate(s)),
                sum(int(x.reset_released) << i for i, x in enumerate(s)),
                s[0].faults,
                s[1].faults,
                s[2].faults,
            )
        )
    return rows


def ramp(start: int, end: int, step: int, **kw) -> list[Step]:
    return [Step(t, **kw) for t in range(start, end + 1, step)]


SCENARIOS = {
    "a_card_in_a_slot_that_is_not_allowed_is_never_powered": ("file_default_all_off", ramp(0, 300, 10, present=0b111)),
    "a_card_is_powered_then_reset_is_released_then_ready": ("all_allowed", [Step(0)] + ramp(10, 200, 5, present=0b001)),
    "reset_is_released_only_after_power_good": ("all_allowed", [Step(0), Step(5, present=1), Step(49, present=1), Step(54, present=1), Step(55, present=1), Step(74, present=1), Step(75, present=1)]),
    "a_fault_during_the_blanking_time_is_ignored_and_after_it_cuts_the_slot": (
        "all_allowed",
        [Step(0), Step(1, present=1), Step(10, present=1, fault=1), Step(20, present=1, fault=1), Step(21, present=1, fault=1)],
    ),
    "a_short_fault_in_ready_is_debounced_and_a_long_one_cuts": (
        "all_allowed",
        ramp(0, 120, 5, present=1) + [Step(125, present=1, fault=1), Step(128, present=1), Step(130, present=1, fault=1), Step(134, present=1, fault=1), Step(135, present=1, fault=1)],
    ),
    "a_slot_in_fault_stays_off_until_it_is_cleared_and_the_fuse_is_quiet": (
        "all_allowed",
        ramp(0, 100, 10, present=1) + [Step(110, present=1, fault=1), Step(115, present=1, fault=1), Step(120, present=1, fault=1, cmd=CLEAR, slot=0), Step(125, present=1, cmd=CLEAR, slot=0), Step(130, present=1)],
    ),
    "pulling_the_card_cuts_the_slot": ("all_allowed", ramp(0, 100, 10, present=0b010) + [Step(110), Step(120)]),
    "three_cards_come_up_independently": ("all_allowed", [Step(0), Step(5, present=0b001), Step(30, present=0b011), Step(60, present=0b111), Step(100, present=0b111), Step(200, present=0b111)]),
    "only_the_allowed_slots_come_up": ("slots_1_and_3", ramp(0, 200, 10, present=0b111)),
    "a_slot_that_keeps_faulting_is_locked_and_only_a_person_unlocks_it": (
        "locks_after_two",
        [Step(0)]
        + ramp(10, 100, 10, present=1)
        + [Step(110, present=1, fault=1), Step(115, present=1, cmd=CLEAR, slot=0), Step(120, present=1)]
        + ramp(125, 220, 5, present=1)
        + [Step(225, present=1, fault=1), Step(230, present=1, cmd=CLEAR, slot=0), Step(240, present=1), Step(250, present=1, cmd=UNLOCK, slot=0)]
        + ramp(255, 400, 5, present=1),
    ),
    "the_clock_wraps": ("all_allowed", [Step(0xFFFFFFE0), Step(0xFFFFFFF0, present=1), Step(0x10, present=1), Step(0x30, present=1), Step(0x50, present=1)]),
}


def fuzz(seed: int, count: int) -> list[Step]:
    rng = random.Random(seed)
    steps, t = [Step(0)], 0
    present = fault = 0
    for _ in range(count):
        t = (t + rng.choice([1, 2, 5, 10, 30])) & 0xFFFFFFFF
        if rng.random() < 0.15:
            present ^= 1 << rng.randrange(3)
        if rng.random() < 0.2:
            fault ^= 1 << rng.randrange(3)
        cmd = rng.choice([NONE] * 6 + [CLEAR, CLEAR, UNLOCK])
        steps.append(Step(t, present, fault, cmd, rng.randrange(3)))
    return steps


SCENARIOS.update(
    {
        "fuzz_seed_1_all_allowed": ("all_allowed", fuzz(1, 400)),
        "fuzz_seed_2_all_allowed": ("all_allowed", fuzz(2, 400)),
        "fuzz_seed_3_locks_after_two": ("locks_after_two", fuzz(3, 400)),
        "fuzz_seed_4_slots_1_and_3": ("slots_1_and_3", fuzz(4, 400)),
    }
)


def render() -> str:
    lines = [
        "/* =============================================================================",
        " * HYDRA-UMC Firmware - tests/expansion_slots_vectors.h",
        " * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>",
        " * GPL-3.0-or-later - see LICENSE",
        " * ============================================================================= */",
        "/* Generated by tools/gen_expansion_slots_vectors.py from tools/expansion_slots.py. Do not edit: change the reference and run the tool. */",
        "#ifndef EXPANSION_SLOTS_VECTORS_H",
        "#define EXPANSION_SLOTS_VECTORS_H",
        "",
        "#include <stdint.h>",
        "",
        "typedef struct { uint8_t allowed_mask; uint32_t fault_blank_ms, power_good_ms, reset_release_ms, fault_debounce_ms; uint8_t max_faults; } es_cfg_t;",
        f"#define ES_CONFIG_COUNT {len(CONFIG_NAMES)}u",
        "static const es_cfg_t es_configs[ES_CONFIG_COUNT] = {",
    ]
    for name in CONFIG_NAMES:
        c = config(name)
        lines.append(f"    {{{c['allowed_mask']}, {c['fault_blank_ms']}u, {c['power_good_ms']}u, {c['reset_release_ms']}u, {c['fault_debounce_ms']}u, {c['max_faults']}}}, /* {name} */")
    lines += [
        "};",
        "typedef struct { uint32_t t; uint8_t present, fault, cmd, slot; } es_in_t;",
        "typedef struct { uint8_t state[3], enable, reset, faults[3]; } es_out_t;",
        "typedef struct { const char *name; uint8_t cfg; unsigned count; const es_in_t *in; const es_out_t *out; } es_scenario_t;",
        "",
    ]
    table = []
    for index, (name, (cfg_name, steps)) in enumerate(SCENARIOS.items()):
        rows = run(cfg_name, steps)
        lines.append(f"static const es_in_t es_in_{index}[] = {{")
        for s in steps:
            lines.append(f"    {{{s.t}u, {s.present}, {s.fault}, {s.cmd}, {s.slot}}},")
        lines += ["};", f"static const es_out_t es_out_{index}[] = {{"]
        for r in rows:
            lines.append(f"    {{{{{r[0]}, {r[1]}, {r[2]}}}, {r[3]}, {r[4]}, {{{r[5]}, {r[6]}, {r[7]}}}}},")
        lines += ["};", ""]
        table.append(f'    {{"{name}", {CONFIG_NAMES.index(cfg_name)}, {len(steps)}u, es_in_{index}, es_out_{index}}},')
    lines += [f"#define ES_SCENARIO_COUNT {len(SCENARIOS)}u", "static const es_scenario_t es_scenarios[ES_SCENARIO_COUNT] = {"] + table + ["};", "", "#endif /* EXPANSION_SLOTS_VECTORS_H */", ""]
    return "\n".join(lines)


def main(argv: list[str]) -> int:
    text = render()
    if "--check" in argv:
        current = HEADER_PATH.read_bytes().decode("utf-8").replace("\r\n", "\n") if HEADER_PATH.is_file() else None
        if current != text:
            print("STALE: tests/expansion_slots_vectors.h is not what the reference says (run tools/gen_expansion_slots_vectors.py)")
            return 1
        return 0
    HEADER_PATH.write_bytes(text.encode("utf-8"))
    print("wrote tests/expansion_slots_vectors.h")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
