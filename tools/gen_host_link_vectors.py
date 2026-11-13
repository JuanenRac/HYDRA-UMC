#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/gen_host_link_vectors.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Writes tests/host_link_vectors.h: scenarios of the transport of the host link run through the Python reference (tools/host_link_master.py), so that the C twin
(src/mcu_stm32h757/Common/host_link_master.h) is held to the same transactions.

    python tools/gen_host_link_vectors.py           write the header
    python tools/gen_host_link_vectors.py --check   exit 1 when the header is not what the reference says

A step is one call of poll at a time, with what the double of the host does in that transaction (nothing, an idle frame, a request, a damaged request, an unsealed request),
whether the peripheral works, the state of the READY line and, optionally, a frame the application stages before the call. After the step the header holds what the poll did, what
the board made of the frame that came in, the type and sequence that went out, the counters and the state. Evidence: simulator."""
from __future__ import annotations

import sys
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import host_link_master as hl  # noqa: E402
import spi_frame as sf  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
HEADER_PATH = ROOT / "tests" / "host_link_vectors.h"

# what the double of the host puts on the line
SILENCE, IDLE, REQUEST, DAMAGED, UNSEALED = 0, 1, 2, 3, 4
NO_ANSWER_BELOW = 0x20  # the dispatch of the test answers requests of 0x20 and above with type ^ 0x40 and the first payload byte


@dataclass(frozen=True)
class Step:
    t: int
    kind: int = SILENCE
    rtype: int = 0
    rseq: int = 0
    rbyte: int = 0
    ready: int = 0
    xfer_ok: int = 1
    stage_type: int = 0  # 0: the application stages nothing before this call
    stage_byte: int = 0


def host_frame(step: Step) -> bytes:
    if step.kind == SILENCE:
        return hl.SILENCE
    if step.kind == IDLE:
        return sf.build(0, 0, hl.IDLE_TYPE, b"", sequence=step.rseq)
    payload = bytes([step.rbyte])
    if step.kind == UNSEALED:
        return sf.build(0, 0, step.rtype, payload)
    wire = bytearray(sf.build(0, 0, step.rtype, payload, sequence=step.rseq))
    if step.kind == DAMAGED:
        wire[sf.OFS_CRC + 1] ^= 0xFF
    return bytes(wire)


def dispatch_double(request: bytes) -> bytes | None:
    if request[2] < NO_ANSWER_BELOW:
        return None
    return bytes([0, 0, request[2] ^ 0x40, 1, request[4]]) + bytes(7)



def configs() -> dict[str, dict]:
    base = hl.load_config()
    return {
        "file": dict(base),
        "ready_wired": dict(base, ready_known=1),
        "tight": dict(base, ready_known=1, max_retries=1, down_after_failures=2, idle_interval_ms=20, min_gap_ms=5),
        "lenient": dict(base, require_seal=0, max_retries=0),
    }


def run(cfg_name: str, steps: list[Step]) -> list[tuple]:
    cfg = configs()[cfg_name]
    current: dict = {}

    def transfer(wire: bytes) -> bytes | None:
        current["sent"] = wire
        return host_frame(current["step"]) if current["step"].xfer_ok else None

    link = hl.HostLinkMaster(cfg, transfer, lambda: bool(current["step"].ready), dispatch_double)
    rows = []
    for step in steps:
        current["step"] = step
        if step.stage_type:
            link.stage(bytes([0, 0, step.stage_type, 1, step.stage_byte]) + bytes(7))
        result = link.poll(step.t)
        c = link.counters
        rows.append(
            (
                result,
                link.last_rx_verdict if result == hl.POLL_DONE else hl.RX_NONE,
                link.last_tx_type if result in (hl.POLL_DONE, hl.POLL_XFER_FAIL) else 0,
                link.last_tx_seq if result in (hl.POLL_DONE, hl.POLL_XFER_FAIL) else 0,
                c.transactions,
                c.dispatched,
                c.duplicates,
                c.bad_crc,
                c.unsealed,
                c.idle_rx,
                c.silence,
                c.transfer_failures,
                c.retries,
                c.dropped,
                int(link.down),
                int(link.staged is not None),
            )
        )
    return rows


def S(t, **kw):
    return Step(t, **kw)


SCENARIOS = {
    # (config, steps)
    "idle_frames_at_the_idle_interval": ("file", [S(0), S(1), S(49), S(50), S(60), S(100), S(151)]),
    "a_request_is_dispatched_and_its_answer_goes_out_on_the_next_transaction": (
        "file",
        [S(0, kind=REQUEST, rtype=0x30, rseq=1, rbyte=7), S(2, kind=IDLE, rseq=2), S(4), S(52)],
    ),
    "a_request_with_no_answer_stages_nothing": ("file", [S(0, kind=REQUEST, rtype=0x10, rseq=1), S(2), S(52)]),
    "a_repeated_sequence_is_not_dispatched_twice": (
        "file",
        [S(0, kind=REQUEST, rtype=0x30, rseq=5, rbyte=1), S(2, kind=REQUEST, rtype=0x30, rseq=5, rbyte=1), S(54, kind=REQUEST, rtype=0x31, rseq=6, rbyte=2)],
    ),
    "a_damaged_frame_is_never_dispatched_and_the_frame_that_went_out_is_sent_again": (
        "file",
        [S(0, stage_type=0x40, stage_byte=9, kind=DAMAGED, rtype=0x30, rseq=1), S(2, kind=DAMAGED, rtype=0x30, rseq=1), S(4, kind=IDLE, rseq=2)],
    ),
    "after_the_retries_the_frame_is_dropped_and_counted": (
        "file",
        [S(0, stage_type=0x40, stage_byte=9, kind=DAMAGED, rtype=0x30, rseq=1)] + [S(2 * i, kind=DAMAGED, rtype=0x30, rseq=1) for i in range(1, 6)] + [S(20, kind=IDLE, rseq=3)],
    ),
    "an_unsealed_frame_is_refused_when_the_seal_is_required": ("file", [S(0, kind=UNSEALED, rtype=0x30, rbyte=3), S(2, kind=REQUEST, rtype=0x30, rseq=1, rbyte=3)]),
    "an_unsealed_frame_is_accepted_when_the_seal_is_not_required": ("lenient", [S(0, kind=UNSEALED, rtype=0x30, rbyte=3), S(2, kind=UNSEALED, rtype=0x30, rbyte=3)]),
    "silence_is_not_an_error_and_a_staged_frame_waits_for_the_idle_interval": (
        "file",
        [S(0, stage_type=0x40, stage_byte=1), S(2), S(10), S(40), S(52), S(54, kind=IDLE, rseq=1), S(56)],
    ),
    "the_peripheral_failing_keeps_the_frame_and_marks_the_link_down_then_up": (
        "tight",
        [S(0, stage_type=0x40, stage_byte=4, xfer_ok=0), S(5, xfer_ok=0), S(10, xfer_ok=0), S(15, kind=IDLE, rseq=1)],
    ),
    "ready_makes_the_board_clock_before_the_idle_interval": ("ready_wired", [S(0), S(6, ready=1, kind=REQUEST, rtype=0x30, rseq=1, rbyte=1), S(8), S(10, ready=1, kind=IDLE, rseq=2), S(14)]),
    "ready_is_not_read_when_the_line_is_not_wired": ("file", [S(0), S(10, ready=1), S(20, ready=1)]),
    "two_transactions_are_never_closer_than_the_minimum_gap": ("tight", [S(0, stage_type=0x40, stage_byte=1), S(1, ready=1), S(4, ready=1), S(5, ready=1, kind=IDLE, rseq=1), S(9, ready=1), S(10, ready=1, kind=IDLE, rseq=2)]),
    "the_clock_wraps": ("file", [S(0xFFFFFFF0), S(0xFFFFFFF5), S(0x15), S(0x20), S(0x48)]),
    "the_sequence_of_the_board_wraps_after_255_frames": ("lenient", [S(i * 50) for i in range(260)]),
    "the_answer_is_staged_after_the_frame_that_went_out_in_the_same_transaction": (
        "file",
        [S(0, stage_type=0x41, stage_byte=1, kind=REQUEST, rtype=0x30, rseq=1, rbyte=8), S(2, kind=IDLE, rseq=2), S(4, kind=IDLE, rseq=3), S(6, kind=IDLE, rseq=4)],
    ),
}


def fuzz(seed: int, count: int) -> list[Step]:
    """A long run of mixed events from a fixed seed: the C twin must make the same call at every step as the reference."""
    import random

    rng = random.Random(seed)
    steps, t, seq = [], 0, 0
    for _ in range(count):
        t = (t + rng.choice([0, 1, 2, 3, 5, 20, 49, 50, 51, 120])) & 0xFFFFFFFF
        kind = rng.choice([SILENCE, IDLE, REQUEST, REQUEST, DAMAGED, UNSEALED])
        if rng.random() < 0.7:
            seq = (seq + 1) & 0xFF  # a new sequence; otherwise the previous one repeats (a retry of the host)
        steps.append(
            Step(t, kind=kind, rtype=rng.choice([0x10, 0x20, 0x30, 0x31, 0x7F]), rseq=seq, rbyte=rng.randrange(256), ready=rng.randrange(2), xfer_ok=int(rng.random() > 0.15),
                 stage_type=rng.choice([0, 0, 0, 0x40, 0x41]), stage_byte=rng.randrange(256))
        )
    return steps


SCENARIOS.update(
    {
        "fuzz_seed_1_file_config": ("file", fuzz(1, 300)),
        "fuzz_seed_2_ready_wired": ("ready_wired", fuzz(2, 300)),
        "fuzz_seed_3_tight": ("tight", fuzz(3, 300)),
        "fuzz_seed_4_lenient": ("lenient", fuzz(4, 300)),
    }
)


def render() -> str:
    cfgs = configs()
    names = list(cfgs)
    lines = [
        "/* =============================================================================",
        " * HYDRA-UMC Firmware - tests/host_link_vectors.h",
        " * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>",
        " * GPL-3.0-or-later - see LICENSE",
        " * ============================================================================= */",
        "/* Generated by tools/gen_host_link_vectors.py from tools/host_link_master.py. Do not edit: change the reference and run the tool. */",
        "#ifndef HOST_LINK_VECTORS_H",
        "#define HOST_LINK_VECTORS_H",
        "",
        "#include <stdint.h>",
        "",
        "typedef struct { uint32_t idle_interval_ms, min_gap_ms; uint8_t max_retries, down_after_failures, require_seal, ready_known; } hl_cfg_t;",
        f"#define HL_CONFIG_COUNT {len(names)}u",
        "static const hl_cfg_t hl_configs[HL_CONFIG_COUNT] = {",
    ]
    for name in names:
        c = cfgs[name]
        lines.append(f"    {{{c['idle_interval_ms']}u, {c['min_gap_ms']}u, {c['max_retries']}, {c['down_after_failures']}, {c['require_seal']}, {c['ready_known']}}}, /* {name} */")
    lines += [
        "};",
        "typedef struct { uint32_t t; uint8_t kind, rtype, rseq, rbyte, ready, xfer_ok, stage_type, stage_byte; } hl_in_t;",
        "typedef struct {",
        "    uint8_t poll, rx, tx_type, tx_seq;",
        "    uint32_t transactions, dispatched, duplicates, bad_crc, unsealed, idle_rx, silence, transfer_failures, retries, dropped;",
        "    uint8_t down, staged;",
        "} hl_out_t;",
        "typedef struct { const char *name; uint8_t cfg; unsigned count; const hl_in_t *in; const hl_out_t *out; } hl_scenario_t;",
        "",
    ]
    table = []
    for index, (name, (cfg_name, steps)) in enumerate(SCENARIOS.items()):
        rows = run(cfg_name, steps)
        lines.append(f"static const hl_in_t hl_in_{index}[] = {{")
        for s in steps:
            lines.append(f"    {{{s.t}u, {s.kind}, {s.rtype}, {s.rseq}, {s.rbyte}, {s.ready}, {s.xfer_ok}, {s.stage_type}, {s.stage_byte}}},")
        lines += ["};", f"static const hl_out_t hl_out_{index}[] = {{"]
        for r in rows:
            lines.append("    {" + f"{r[0]}, {r[1]}, {r[2]}, {r[3]}, " + ", ".join(f"{v}u" for v in r[4:14]) + f", {r[14]}, {r[15]}" + "},")
        lines += ["};", ""]
        table.append(f'    {{"{name}", {names.index(cfg_name)}, {len(steps)}u, hl_in_{index}, hl_out_{index}}},')
    lines += [f"#define HL_SCENARIO_COUNT {len(SCENARIOS)}u", "static const hl_scenario_t hl_scenarios[HL_SCENARIO_COUNT] = {"] + table + ["};", "", "#endif /* HOST_LINK_VECTORS_H */", ""]
    return "\n".join(lines)


def main(argv: list[str]) -> int:
    text = render()
    if "--check" in argv:
        current = HEADER_PATH.read_bytes().decode("utf-8").replace("\r\n", "\n") if HEADER_PATH.is_file() else None
        if current != text:
            print("STALE: tests/host_link_vectors.h is not what the reference says (run tools/gen_host_link_vectors.py)")
            return 1
        return 0
    HEADER_PATH.write_bytes(text.encode("utf-8"))
    print("wrote tests/host_link_vectors.h")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
