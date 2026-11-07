# =============================================================================
# HYDRA-UMC - tests/sim_can_bus.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""A CAN bus emulated in one process, in steps of one millisecond, for the host tests of the bus contract. No kernel module, no board.

What it models: arbitration (the lowest identifier goes first), a capacity per millisecond that follows from the bit rate and the worst
case frame length, two nodes sending the same identifier with different data in the same step (both frames are lost and an error frame is
counted for every listener), error injection (a frame lost, a node forced bus-off) and the rule of the identifier table (one emitter per
identifier: a node that sends an identifier of another emitter is recorded as a violation). What it does not model: bit timing, the real
error counters of a controller, the electrical layer. Every number that comes out of it is a simulation: evidence = simulator."""
from __future__ import annotations

import sys
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

import can_ids  # noqa: E402
import estop_can  # noqa: E402
import slot_scan  # noqa: E402


@dataclass(frozen=True)
class Frame:
    can_id: int
    data: bytes


class VirtualCanBus:
    def __init__(self, bitrate: int = 1_000_000):
        self.bitrate = bitrate
        self.frames_per_ms = max(1, bitrate // 1000 // estop_can.worst_case_frame_bits())
        self.now_ms = 0
        self.nodes: list["SimNode"] = []
        self._pending: list[tuple["SimNode", Frame]] = []
        self.log: list[tuple[int, int, bytes]] = []  # (time, identifier, data) of every frame that went through
        self.error_frames: list[tuple[int, int]] = []  # (time, identifier) of every collision
        self.violations: list[str] = []
        self.drop_next: dict[int, int] = defaultdict(int)  # identifier -> number of frames to lose

    def attach(self, node: "SimNode") -> "SimNode":
        node.bus = self
        self.nodes.append(node)
        return node

    def transmit(self, node: "SimNode", frame: Frame) -> None:
        if not node.bus_off:
            self._pending.append((node, frame))

    def step(self) -> None:
        """One millisecond: the nodes that wait for this time send, then the bus carries what its capacity allows, lowest identifier first."""
        for node in list(self.nodes):
            node.tick(self.now_ms)
        capacity = self.frames_per_ms
        carried: list[tuple["SimNode", Frame]] = []
        self._pending.sort(key=lambda item: item[1].can_id)
        remaining: list[tuple["SimNode", Frame]] = []
        while self._pending:
            node, frame = self._pending.pop(0)
            same = [(n, f) for n, f in self._pending if f.can_id == frame.can_id and n is not node]
            if same and any(f.data != frame.data for _, f in same):
                self.error_frames.append((self.now_ms, frame.can_id))
                self._pending = [(n, f) for n, f in self._pending if f.can_id != frame.can_id]
                for listener in self.nodes:
                    listener.on_error_frame(self.now_ms, frame.can_id)
                continue
            if capacity == 0:
                remaining.append((node, frame))
                continue
            capacity -= 1
            carried.append((node, frame))
        self._pending = remaining
        for node, frame in carried:
            self._check_emitter(node, frame)
            if self.drop_next[frame.can_id] > 0:
                self.drop_next[frame.can_id] -= 1
                continue
            self.log.append((self.now_ms, frame.can_id, frame.data))
            for listener in list(self.nodes):
                if listener is not node and not listener.bus_off:
                    listener.on_frame(self.now_ms, frame)
        self.now_ms += 1

    def run(self, milliseconds: int) -> None:
        for _ in range(milliseconds):
            self.step()

    def _check_emitter(self, node: "SimNode", frame: Frame) -> None:
        if node.allowed_ids is not None and frame.can_id not in node.allowed_ids:
            self.violations.append(f"{node.name} sent 0x{frame.can_id:03X}, which is not one of its identifiers")


@dataclass
class SimNode:
    name: str
    allowed_ids: set[int] | None = None
    bus: VirtualCanBus | None = None
    bus_off: bool = False
    errors_seen: int = 0

    def send(self, can_id: int, data: bytes) -> None:
        assert self.bus is not None
        self.bus.transmit(self, Frame(can_id, data))

    def on_frame(self, now_ms: int, frame: Frame) -> None:
        pass

    def on_error_frame(self, now_ms: int, can_id: int) -> None:
        self.errors_seen += 1

    def tick(self, now_ms: int) -> None:
        pass

    def go_bus_off(self) -> None:
        self.bus_off = True


def slot_base(slot: int) -> int:
    return slot * can_ids.SLOT_WINDOW


class VirtualBoard(SimNode):
    """A board with the firmware logic of the slot scan: it answers the probe of its own slot window after the delay its unique identifier gives."""

    def __init__(self, slot: int, board_id: int, uid32: int, role: int = slot_scan.ROLE_BOOTLOADER, *, name: str | None = None):
        reply_id = can_ids.identifier(slot_base(slot), can_ids.by_name("SLOT_SCAN_REPLY"))
        super().__init__(name or f"board slot {slot} uid {uid32:08x}", {reply_id})
        self.slot, self.board_id, self.uid32, self.role = slot, board_id, uid32, role
        self._answers: list[tuple[int, Frame]] = []

    def on_frame(self, now_ms: int, frame: Frame) -> None:
        probe_id = can_ids.identifier(slot_base(self.slot), can_ids.by_name("SLOT_SCAN_PROBE"))
        scan_id = slot_scan.decode_probe(frame.data) if frame.can_id == probe_id else None
        if scan_id is None:
            return
        reply_id = can_ids.identifier(slot_base(self.slot), can_ids.by_name("SLOT_SCAN_REPLY"))
        data = slot_scan.encode_reply(self.board_id, self.role, scan_id, self.uid32)
        self._answers.append((now_ms + slot_scan.reply_delay_ms(self.uid32) + 1, Frame(reply_id, data)))

    def tick(self, now_ms: int) -> None:
        due = [a for a in self._answers if a[0] <= now_ms]
        self._answers = [a for a in self._answers if a[0] > now_ms]
        for _, frame in due:
            self.send(frame.can_id, frame.data)


class ScanHost(SimNode):
    """The host side of the slot scan: sends the probe to every slot window, listens for the scan window and classifies what it heard."""

    def __init__(self):
        probe_ids = {can_ids.identifier(slot_base(s), can_ids.by_name("SLOT_SCAN_PROBE")) for s in range(slot_scan.SLOTS)}
        super().__init__("host", probe_ids)
        self.heard: dict[int, list[bytes]] = defaultdict(list)
        self.errors: dict[int, int] = defaultdict(int)

    def on_frame(self, now_ms: int, frame: Frame) -> None:
        self.heard[frame.can_id // can_ids.SLOT_WINDOW].append(frame.data)

    def on_error_frame(self, now_ms: int, can_id: int) -> None:
        self.errors[can_id // can_ids.SLOT_WINDOW] += 1

    def scan(self, scan_id: int) -> dict[int, str]:
        assert self.bus is not None
        self.heard.clear()
        self.errors.clear()
        for slot in range(slot_scan.SLOTS):
            self.send(can_ids.identifier(slot_base(slot), can_ids.by_name("SLOT_SCAN_PROBE")), slot_scan.encode_probe(scan_id))
        self.bus.run(slot_scan.load_contract()["timing_ms"]["scan_window"])
        heard = {
            slot: slot_scan.Heard(
                tuple(f for f in self.heard[slot] if f[:1] == bytes([int(slot_scan.load_contract()["reply_magic"], 16)])),
                self.errors[slot],
            )
            for slot in range(slot_scan.SLOTS)
        }
        return slot_scan.scan(heard, scan_id)


@dataclass
class EstopListener(SimNode):
    """A node that moves or heats something: the reference state machine of docs/ESTOP_CAN.md on a virtual bus."""

    node: estop_can.Node = field(default_factory=estop_can.Node)

    def on_frame(self, now_ms: int, frame: Frame) -> None:
        if frame.can_id == estop_can.ESTOP_CAN_ID:
            self.node.on_frame(now_ms, frame.data)

    def tick(self, now_ms: int) -> None:
        self.node.tick(now_ms)

    def go_bus_off(self) -> None:
        super().go_bus_off()
        self.node.on_bus_off()


Listener = Callable[[int, Frame], None]
