# =============================================================================
# HYDRA-UMC - tests/sim_hil_bench.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""A virtual bench for the host-in-the-loop scripts of docs/BENCH_TESTS.md: doubles of the controller board and of the kinematic brain on the virtual
CAN bus of tests/sim_can_bus.py, the batch orchestrator that flashes several boards one after the other, and the SPI side of the CM5.

What the doubles are: the Python mirror of the logic that the host tests run in C (app_boot_request.h, health_frame.h, spi_system.h, estop_can.h,
slot_scan_node.h). They answer the same frames with the same rules; they have no timing of the chip, no HAL, no flash. A bench row stays open until
it is run on the boards: this module only makes the scripts of the rows runnable and checkable today. Evidence: simulator."""
from __future__ import annotations

import sys
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

import can_ids  # noqa: E402
import estop_can  # noqa: E402
import health_frame  # noqa: E402
import slot_scan  # noqa: E402
import spi_system  # noqa: E402
from sim_can_bus import Frame, ScanHost, SimNode, VirtualBoard, VirtualCanBus, slot_base  # noqa: E402

ENTER_BOOTLOADER_OFFSET = 0x00
ENTER_BOOTLOADER_MAGIC = bytes((0xB0, 0x07, 0x1D, 0x5A))
REBOOT_MS = 40  # how long a board is silent while it resets into its other program (a model value, not a measurement)


@dataclass
class BoardOptions:
    """Switches of the double, the same ones the firmware has (all off, as shipped, unless a test turns them on)."""

    enter_bootloader: bool = False   # HYDRA_APP_ENTER_BOOTLOADER
    health_frame: bool = False       # HYDRA_HEALTH_FRAME
    deaf: bool = False               # a board that hears nothing (a broken receiver)


class ControllerBoardDouble(VirtualBoard):
    """A controller board in either of its two programs. In the application it keeps the emergency stop state, answers ENTER_BOOTLOADER the way
    app_boot_request.h does and sends its health frame; in the bootloader it only answers the slot scan and can be flashed."""

    def __init__(self, slot: int, uid32: int, options: BoardOptions | None = None, *, version: str = "0.1.0", role: int = slot_scan.ROLE_APPLICATION):
        super().__init__(slot, slot, uid32, role, name=f"board {slot}")
        self.options = options or BoardOptions()
        self.node = estop_can.Node()
        self.version = version
        self.bootloader_version = "0.1.0"
        self.reboot_until: int | None = None
        self.next_role: int | None = None
        self.health = health_frame.Sender()
        self.resets = 0
        self.allowed_ids = {
            can_ids.identifier(slot_base(slot), can_ids.by_name("SLOT_SCAN_REPLY")),
            can_ids.identifier(slot_base(slot), can_ids.by_name("HEALTH_FRAME")),
        }
        self.health_id = can_ids.identifier(slot_base(slot), can_ids.by_name("HEALTH_FRAME"))
        self.enter_id = slot_base(slot) + ENTER_BOOTLOADER_OFFSET
        self.flash_writes = 0

    @property
    def silent(self) -> bool:
        return self.reboot_until is not None

    def on_frame(self, now_ms: int, frame: Frame) -> None:
        if self.options.deaf or self.silent:
            return
        if frame.can_id == estop_can.ESTOP_CAN_ID and self.role == slot_scan.ROLE_APPLICATION:
            self.node.on_frame(now_ms, frame.data)
            return
        if frame.can_id == self.enter_id and self.role == slot_scan.ROLE_APPLICATION:
            self._on_enter_bootloader(now_ms, frame.data)
            return
        super().on_frame(now_ms, frame)

    def _on_enter_bootloader(self, now_ms: int, data: bytes) -> None:
        # app_boot_request.h: off -> ignored; exact magic only; never while the drivers are enabled
        if not self.options.enter_bootloader or data != ENTER_BOOTLOADER_MAGIC or self.node.drivers_enabled:
            return
        self.reset_into(slot_scan.ROLE_BOOTLOADER, now_ms)

    def reset_into(self, role: int, now_ms: int) -> None:
        self.resets += 1
        self.reboot_until = now_ms + REBOOT_MS
        self.next_role = role
        self._answers.clear()

    def tick(self, now_ms: int) -> None:
        if self.reboot_until is not None:
            if now_ms < self.reboot_until:
                return
            self.reboot_until = None
            self.role = self.next_role if self.next_role is not None else self.role
            self.node = estop_can.Node()   # a start is always latched
            return
        super().tick(now_ms)
        if self.options.health_frame and self.role == slot_scan.ROLE_APPLICATION:
            self.health.flags = (health_frame.FLAG_ESTOP if self.node.state == estop_can.LATCHED else 0) | (
                health_frame.FLAG_DRIVERS_ENABLED if self.node.drivers_enabled else 0)
            frame = self.health.due(now_ms)
            if frame is not None:
                self.send(self.health_id, frame)

    # the flash of the bootloader: abstract (the byte-level cycle is tested in C and in the bridge tests)
    def flash(self, new_version: str, *, cut_after_fraction: float | None = None) -> str:
        """'ok', or 'cut' when the power goes before the metadata record is written: the old application stays, as the append-only log guarantees."""
        if self.role != slot_scan.ROLE_BOOTLOADER:
            return "not_in_bootloader"
        self.flash_writes += 1
        if cut_after_fraction is not None and cut_after_fraction < 1.0:
            return "cut"
        self.version = new_version
        return "ok"


class ScanBrain(ScanHost):
    """The host side on the bus, standing for the kinematic brain and the CM5 bridge: it scans the slots, sends ENTER_BOOTLOADER and the frames of the
    emergency stop, and keeps the health frames it hears."""

    def __init__(self) -> None:
        super().__init__()
        self.allowed_ids = (self.allowed_ids or set()) | {estop_can.ESTOP_CAN_ID} | {slot_base(s) + ENTER_BOOTLOADER_OFFSET for s in range(slot_scan.SLOTS)}
        self.health_heard: dict[int, list[health_frame.Health]] = {}

    def on_frame(self, now_ms: int, frame: Frame) -> None:
        super().on_frame(now_ms, frame)
        if frame.can_id % can_ids.SLOT_WINDOW == health_frame.OFFSET:
            decoded = health_frame.decode(frame.data)
            if decoded is not None:
                self.health_heard.setdefault(frame.can_id // can_ids.SLOT_WINDOW, []).append(decoded)


# --- the batch orchestrator (bench rows 14 and 15) ---------------------------------------------------------------------------------------------------


@dataclass
class BatchResult:
    slot: int
    status: str            # updated | skipped | no_answer | flash_failed
    reason: str = ""
    version_before: str = ""
    version_after: str = ""


@dataclass
class Bench:
    bus: VirtualCanBus
    host: ScanBrain
    boards: list[ControllerBoardDouble] = field(default_factory=list)
    scan_counter: int = 0

    def add(self, board: ControllerBoardDouble) -> ControllerBoardDouble:
        self.bus.attach(board)
        self.boards.append(board)
        return board

    def scan(self) -> dict[int, str]:
        self.scan_counter = (self.scan_counter + 1) % 250 + 1
        return self.host.scan(self.scan_counter)

    def boards_in(self, slot: int) -> list[ControllerBoardDouble]:
        return [b for b in self.boards if b.slot == slot]


def make_bench() -> Bench:
    bus = VirtualCanBus()
    host = ScanBrain()
    bus.attach(host)
    return Bench(bus, host)


def flash_batch(bench: Bench, plan: dict[int, str], *, enter_wait_ms: int = 3 * REBOOT_MS, cuts: dict[int, float] | None = None) -> list[BatchResult]:
    """Update the boards of `plan` (slot -> new version) one after the other.

    1. scan: a slot whose verdict is not OK is skipped before anything is sent to it (a duplicate is reported, never written);
    2. ENTER_BOOTLOADER to every slot that is OK and runs its application; wait; scan again: a slot that did not come back as a bootloader is reported;
    3. flash the slots that are now bootloaders, one by one; a cut leaves the old application, the board is reported as failed;
    4. a last scan: a board that was flashed must answer again, as an application, with its new version."""
    cuts = cuts or {}
    results: dict[int, BatchResult] = {}
    first = bench.scan()
    for slot, new_version in plan.items():
        boards = bench.boards_in(slot)
        before = boards[0].version if boards else ""
        if first[slot] != slot_scan.OK:
            results[slot] = BatchResult(slot, "skipped", f"scan says {first[slot]}", before)
    candidates = [s for s in plan if s not in results]
    for slot in candidates:
        bench.host.send(slot_base(slot) + ENTER_BOOTLOADER_OFFSET, ENTER_BOOTLOADER_MAGIC)
    bench.bus.run(enter_wait_ms)
    second = bench.scan()
    for slot in candidates:
        board = bench.boards_in(slot)[0]
        if board.role != slot_scan.ROLE_BOOTLOADER or second[slot] != slot_scan.OK:
            results[slot] = BatchResult(slot, "no_answer", "the board did not enter its bootloader", board.version)
            continue
        outcome = board.flash(plan[slot], cut_after_fraction=cuts.get(slot))
        if outcome != "ok":
            board.reset_into(slot_scan.ROLE_APPLICATION, bench.bus.now_ms)   # a cut board restarts into the application it still has
            results[slot] = BatchResult(slot, "flash_failed", outcome, board.version)
            continue
        board.reset_into(slot_scan.ROLE_APPLICATION, bench.bus.now_ms)
        results[slot] = BatchResult(slot, "updated", "", "", board.version)
    bench.bus.run(enter_wait_ms)
    third = bench.scan()
    for slot, result in results.items():
        if result.status == "updated" and (third[slot] != slot_scan.OK or bench.boards_in(slot)[0].role != slot_scan.ROLE_APPLICATION):
            results[slot] = BatchResult(slot, "flash_failed", "the board did not answer after the update", "", result.version_after)
    return [results[s] for s in plan]


# --- the SPI side of the CM5 (bench rows 13, 19) ----------------------------------------------------------------------------------------------------


class SpiBrainDouble:
    """The CM4 as the CM5 sees it over the SPI link: the commands of spi_system.h on top of the real bus. It owns the brain-side emergency stop."""

    def __init__(self, bench: Bench, config: spi_system.Config):
        self.bench = bench
        self.config = config
        self.snapshot = spi_system.Snapshot()
        self.tripped = True
        self.trips: list[int] = []
        self.releases = 0
        self.scans = 0

    def exchange(self, frame_type: int, payload: bytes) -> tuple[int, bytes]:
        action, data = spi_system.handle(self.config, self.snapshot, frame_type, payload)
        if action == "reply":
            return spi_system.TELEMETRY_RESPONSE, data
        if action == "trip":
            self.tripped = True
            self.trips.append(data[0])
        elif action == "release":
            self.releases += 1
        elif action == "scan":
            self.scans += 1
            result = self.bench.scan()
            self.snapshot.scan_verdicts = [spi_system.VERDICTS[result[slot]] for slot in range(slot_scan.SLOTS)]
            return spi_system.PROBE_RESPONSE, spi_system.probe_response(self.snapshot, data[0])
        return 0, b""


# --- the frame of the safety-zones service on the way to the emergency stop (HSAF-IDEA-005) ---------------------------------------------------------


def crc8(data: bytes) -> int:
    """CRC-8, polynomial 0x07, start 0 (the check byte of the frames the safety-zones service proposes)."""
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


ZONES_KIND_STOP = 0x01
ZONES_KIND_HEARTBEAT = 0x02


def zones_frame(kind: int, sequence: int, subject: int = 0, breached: int = 0) -> bytes:
    """The 8 bytes of the service: kind, version 1, sequence (16 bits little endian), subject (16 bits), flags (number of zones breached), CRC-8."""
    body = bytes((kind, 1, sequence & 0xFF, (sequence >> 8) & 0xFF, subject & 0xFF, (subject >> 8) & 0xFF, breached & 0xFF))
    return body + bytes((crc8(body),))


class ZonesBridge(SimNode):
    """The translation the CM5 bridge does: the safety-zones service talks to the CM5 on its own identifiers (never on 0x001, whose layout is the one
    of docs/ESTOP_CAN.md) and the bridge puts a TRIP of reason 'safety-zone' on the stack bus when it hears a stop, a damaged frame, or no heartbeat
    for the maximum age. It only ever requests a stop: the boards decide, and a release stays with the brain and a person."""

    REASON_SAFETY_ZONE = 0x04

    def __init__(self, stop_id: int = 0x7A0, heartbeat_id: int = 0x7A1, max_age_ms: int = 1000, trip_repeat_ms: int = 5, repeats: int = 3):
        super().__init__("zones bridge", {estop_can.ESTOP_CAN_ID})
        self.stop_id, self.heartbeat_id = stop_id, heartbeat_id
        self.max_age_ms, self.trip_repeat_ms, self.repeats = max_age_ms, trip_repeat_ms, repeats
        self.last_heartbeat_ms: int | None = None
        self.seq = 0
        self.epoch = 1
        self.tripped_reason: str | None = None
        self._pending = 0
        self._next_ms = 0

    def on_frame(self, now_ms: int, frame: Frame) -> None:
        if frame.can_id not in (self.stop_id, self.heartbeat_id):
            return
        valid = len(frame.data) == 8 and frame.data[7] == crc8(frame.data[:7]) and frame.data[1] == 1
        if not valid:
            self._trip(now_ms, "damaged frame")
        elif frame.can_id == self.stop_id and frame.data[0] == ZONES_KIND_STOP:
            self._trip(now_ms, "stop request")
        elif frame.can_id == self.heartbeat_id and frame.data[0] == ZONES_KIND_HEARTBEAT:
            self.last_heartbeat_ms = now_ms

    def _trip(self, now_ms: int, why: str) -> None:
        if self.tripped_reason is None:
            self.tripped_reason = why
        self._pending = self.repeats
        self._next_ms = now_ms

    def tick(self, now_ms: int) -> None:
        if self.last_heartbeat_ms is not None and self.tripped_reason is None and now_ms - self.last_heartbeat_ms > self.max_age_ms:
            self._trip(now_ms, "no heartbeat")
        if self._pending and now_ms >= self._next_ms:
            self.seq += 1
            self.send(estop_can.ESTOP_CAN_ID, estop_can.encode(estop_can.KIND_TRIP, self.REASON_SAFETY_ZONE, estop_can.SOURCE_CM5, self.epoch, self.seq))
            self._pending -= 1
            self._next_ms = now_ms + self.trip_repeat_ms
