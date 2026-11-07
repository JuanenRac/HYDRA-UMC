# =============================================================================
# HYDRA-UMC - tests/vcan_check.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The bus contract over a virtual CAN interface of the Linux kernel (started by .github/workflows/vcan-check.yml, Linux only).

    python3 tests/vcan_check.py vcan0

Two raw sockets stand for two nodes of the stack. The test sends the frames of the contract through the kernel and reads them back: the emergency
stop (trip, alive, release) with the node state machine of tools/estop_can.py, the slot scan probe and reply, and the health frame. It checks that the
identifiers and the layouts survive a real CAN stack unchanged. A virtual interface has no arbitration and no bit timing: this is a check of the
tables and the layouts, not of a bus. Evidence: simulator (kernel virtual interface)."""
from __future__ import annotations

import socket
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

import can_ids  # noqa: E402
import estop_can  # noqa: E402
import health_frame  # noqa: E402
import slot_scan  # noqa: E402

FRAME_FORMAT = "=IB3x8s"   # struct can_frame: id, dlc, padding, 8 data bytes
FRAME_SIZE = struct.calcsize(FRAME_FORMAT)


def open_socket(interface: str) -> socket.socket:
    sock = socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
    sock.bind((interface,))
    sock.settimeout(1.0)
    return sock


def send(sock: socket.socket, can_id: int, data: bytes) -> None:
    sock.send(struct.pack(FRAME_FORMAT, can_id, len(data), data.ljust(8, b"\x00")))


def receive(sock: socket.socket) -> tuple[int, bytes]:
    raw = sock.recv(FRAME_SIZE)
    can_id, dlc, data = struct.unpack(FRAME_FORMAT, raw)
    return can_id & 0x7FF, data[:dlc]


def expect(condition: bool, message: str, failures: list[str]) -> None:
    print(("PASS " if condition else "FAIL ") + message)
    if not condition:
        failures.append(message)


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print("usage: vcan_check.py <interface>")
        return 2
    failures: list[str] = []
    brain, board = open_socket(argv[1]), open_socket(argv[1])
    brain.setsockopt(socket.SOL_CAN_RAW, socket.CAN_RAW_RECV_OWN_MSGS, 0)
    board.setsockopt(socket.SOL_CAN_RAW, socket.CAN_RAW_RECV_OWN_MSGS, 0)

    # the emergency stop: the node reads every frame from its socket and runs the reference state machine
    node = estop_can.Node()
    now = 0
    for step, (kind, detail) in enumerate(((estop_can.KIND_TRIP, 0x01), (estop_can.KIND_ALIVE, estop_can.ALIVE_OK), (estop_can.KIND_ALIVE, estop_can.ALIVE_OK),
                                           (estop_can.KIND_RELEASE, 0))):
        send(brain, estop_can.ESTOP_CAN_ID, estop_can.encode(kind, detail, estop_can.SOURCE_BRAIN, 1, step + 1))
        can_id, data = receive(board)
        expect(can_id == estop_can.ESTOP_CAN_ID and len(data) == 8, f"emergency stop frame {step} keeps its identifier and its 8 bytes", failures)
        now += 10
        node.on_frame(now, data)
    expect(node.state == estop_can.READY and not node.drivers_enabled, "trip, alive, alive, release leaves the node READY with the drivers off", failures)
    send(brain, estop_can.ESTOP_CAN_ID, estop_can.encode(estop_can.KIND_TRIP, 0x02, estop_can.SOURCE_BRAIN, 2, 5))
    node.on_frame(now + 1, receive(board)[1])
    expect(node.state == estop_can.LATCHED, "a trip latches the node", failures)
    send(brain, estop_can.ESTOP_CAN_ID, b"\x01\x02\x03")
    can_id, data = receive(board)
    expect(estop_can.decode(data) is None, "a frame of 3 bytes arrives as 3 bytes and is malformed for the reference", failures)

    # the slot scan: a probe to the window of slot 5 and the reply from the board
    base = 5 * can_ids.SLOT_WINDOW + 0x600
    probe_id = can_ids.identifier(base, can_ids.by_name("SLOT_SCAN_PROBE"))
    reply_id = can_ids.identifier(base, can_ids.by_name("SLOT_SCAN_REPLY"))
    send(brain, probe_id, slot_scan.encode_probe(42))
    can_id, data = receive(board)
    expect(can_id == probe_id and slot_scan.decode_probe(data) == 42, "the probe reaches the window of the slot with its scan id", failures)
    send(board, reply_id, slot_scan.encode_reply(5, slot_scan.ROLE_APPLICATION, 42, 0xCAFEF00D))
    can_id, data = receive(brain)
    reply = slot_scan.decode_reply(data)
    expect(can_id == reply_id and reply is not None and reply.uid32 == 0xCAFEF00D and reply.board_id == 5, "the reply carries the board and its unique identifier", failures)

    # the health frame
    health_id = can_ids.identifier(base, can_ids.by_name("HEALTH_FRAME"))
    sent = health_frame.build(7, health_frame.FLAG_ESTOP, 1, 2, 0, 20_000)
    send(board, health_id, sent)
    can_id, data = receive(brain)
    decoded = health_frame.decode(data)
    expect(can_id == health_id and decoded is not None and decoded.sequence == 7 and decoded.uptime_s == 16, "the health frame survives the kernel unchanged", failures)

    # every identifier of the table fits in 11 bits for the last slot and is delivered with the right length
    for message in can_ids.MESSAGES:
        identifier = can_ids.identifier(0x600 + 7 * can_ids.SLOT_WINDOW, message)
        send(brain, identifier, bytes(range(8)))
        got_id, got = receive(board)
        expect(got_id == identifier and got == bytes(range(8)), f"{message.name} (0x{identifier:03X}) is delivered unchanged", failures)

    print(f"VCAN_CHECK={'PASS' if not failures else 'FAIL'} ({len(failures)} failures)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
