# =============================================================================
# HYDRA-UMC - tests/sim_timing_models.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Time models of three places where the firmware has a timing question that only the bench can settle (docs/BENCH_TESTS.md rows 12, 13 and 19).

The parameters are in tests/timing_models.json, not in this file. A model answers "what happens if the numbers are these", so a bench measurement
replaces a number and the answer follows. Nothing here is a measurement: every result is evidence = simulator.

    g474_start      frames that arrive at the controller board between the start of its bus and the first poll of the relay task, and the
                    SysTick ticks that run before the scheduler, for each value of HYDRA_QUIET_TICK_BEFORE_SCHEDULER
    spi_link        the 128-byte frame of the SPI link at full rate with damaged and repeated frames, run through the real receiver rule of
                    tools/spi_frame.py: what is dispatched, what is dropped, and whether the link recovers on the next frame
    stack_a_relay   how long one status read of a board takes through the relay of the CM4 (request and two fragments on the bus, plus the wait for
                    the relay task of the board) and how often the eight boards can be read
"""
from __future__ import annotations

import json
import random
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

import spi_frame  # noqa: E402

MODELS = json.loads((Path(__file__).resolve().parent / "timing_models.json").read_text(encoding="utf-8"))


def frame_time_us(bits: int, bit_rate_bps: int) -> float:
    return bits * 1_000_000 / bit_rate_bps


def g474_start(quiet_tick_level: int = 0, params: dict | None = None) -> dict:
    p = params or MODELS["g474_start"]
    window_ms = p["task_creation_ms"] + p["first_poll_after_scheduler_ms"]   # from the bus coming up (end of the set-up) to the first poll
    arrived = int(window_ms * 1000 // frame_time_us(p["worst_frame_bits"], p["bus_bit_rate_bps"]))
    kept = min(arrived, p["rx_fifo_depth"])
    total_ms = p["set_up_ms"] + p["task_creation_ms"]                        # from HAL_Init to the scheduler
    ticks = {0: total_ms, 1: p["set_up_ms"], 2: 0}[quiet_tick_level]
    return {
        "frames_arrived": arrived,
        "frames_kept_in_the_fifo": kept,
        "frames_lost": arrived - kept,
        "ticks_before_the_scheduler": ticks,
        "receive_interrupts_before_the_scheduler": 0,   # the relay polls: no FDCAN interrupt is enabled (tests/test_hy25_sources.py keeps it so)
        "reset_cycles": p["reset_cycles"],
    }


def spi_link(params: dict | None = None, seed: int = 20260101) -> dict:
    p = params or MODELS["spi_link"]
    rng = random.Random(seed)
    wire_us = p["frame_bytes"] * 8 * 1_000_000 / p["clock_hz"]
    period_us = wire_us + p["cm4_service_us"]
    sent = delivered = dropped_bad = dropped_duplicate = dropped_unsealed = accepted_unchecked = recovered = retries = 0
    last: int | None = None
    pending_recovery = False
    sequence = 0
    frames_to_send = p["frames_to_send"]
    while delivered < frames_to_send:
        sent += 1
        wire = bytearray(spi_frame.build(0, 1, 0x05, bytes((sent & 0xFF,)), sequence))
        damaged = (sent % p["damage_every_frames"]) == 0
        if damaged:
            wire[rng.randrange(0, spi_frame.CRC_SPAN + 2)] ^= 1 << rng.randrange(8)   # one bit of the covered bytes or of the CRC itself
        verdict = spi_frame.judge(bytes(wire), last)
        if verdict == "dispatch":
            delivered += 1
            last = sequence
            sequence = (sequence + 1) & 0xFF
            if pending_recovery:
                recovered += 1
                pending_recovery = False
        elif verdict == "bad_crc":
            dropped_bad += 1
            retries += 1                    # the sender repeats the same sequence
            pending_recovery = True
        elif verdict == "duplicate":
            dropped_duplicate += 1
        elif verdict == "unsealed":
            # a bit flip in the marker makes a sealed frame look like the older kind without a seal: accepted unchecked unless HYDRA_SPI_REQUIRE_CRC is 1
            if p["require_crc"]:
                dropped_unsealed += 1
                retries += 1
                pending_recovery = True
            else:
                delivered += 1
                accepted_unchecked += 1
                sequence = (sequence + 1) & 0xFF
        else:
            raise AssertionError(verdict)
        if rng.random() < 0.02 and last is not None:   # the sender repeats a frame that was in fact received
            repeat = spi_frame.build(0, 1, 0x05, b"\x00", last)
            if spi_frame.judge(repeat, last) == "duplicate":
                dropped_duplicate += 1
            sent += 1
    return {
        "frames_on_the_wire": sent,
        "frames_dispatched": delivered,
        "dropped_bad_crc": dropped_bad,
        "dropped_duplicates": dropped_duplicate,
        "dropped_unsealed": dropped_unsealed,
        "accepted_unsealed_unchecked": accepted_unchecked,
        "retries": retries,
        "recoveries": recovered,
        "frame_period_us": round(period_us, 1),
        "frames_per_second_at_full_rate": int(1_000_000 // period_us),
    }


def stack_a_relay(params: dict | None = None) -> dict:
    p = params or MODELS["stack_a_relay"]
    bus_us = frame_time_us(p["worst_frame_bits"], p["bus_bit_rate_bps"])
    on_bus_ms = bus_us * (1 + p["frames_per_status_read"]) / 1000
    best = on_bus_ms
    worst = on_bus_ms + p["relay_task_period_ms"]
    average = on_bus_ms + p["relay_task_period_ms"] / 2
    worst_while_scan = on_bus_ms + p["relay_task_period_while_scan_ms"]
    return {
        "bus_time_ms": round(on_bus_ms, 3),
        "read_best_ms": round(best, 3),
        "read_average_ms": round(average, 3),
        "read_worst_ms": round(worst, 3),
        "read_worst_during_scan_ms": round(worst_while_scan, 3),
        "all_boards_worst_ms": round(worst * p["boards"], 3),
        "refresh_rate_hz_all_boards_worst_case": round(1000 / (worst * p["boards"]), 2),
    }
