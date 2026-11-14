# =============================================================================
# HYDRA-UMC - tests/vcan_urtc_vectors.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The CAN protocol vectors of the URTC tool heads over a virtual CAN interface of the Linux kernel (started by .github/workflows/vcan-check.yml, Linux only).

    python3 tests/vcan_urtc_vectors.py vcan0

tests/urtc_can_conformance_vectors.json is a copy of the shared vectors every URTC client reproduces byte for byte (82 identifiers, the magic payloads of the
bootloader, the frames of the answers of a tool head, the frames of a version query). Two raw sockets stand for the stack and a tool head: every identifier and
every frame of the vectors goes through the kernel and has to arrive with the same identifier, the same length and the same bytes, and the version query is
delivered in the order it was sent. A virtual interface has no arbitration and no bit timing:
this checks that the identifiers and the layouts of the vectors survive a real CAN stack, not a bus and not a board. Evidence: simulator (kernel virtual interface)."""
from __future__ import annotations

import json
import socket
import struct
import sys
from pathlib import Path

VECTORS = Path(__file__).resolve().parent / "urtc_can_conformance_vectors.json"
FRAME_FORMAT = "=IB3x8s"   # struct can_frame: id, dlc, padding, 8 data bytes
FRAME_SIZE = struct.calcsize(FRAME_FORMAT)


def open_socket(interface: str) -> socket.socket:
    sock = socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
    sock.bind((interface,))
    sock.settimeout(1.0)
    sock.setsockopt(socket.SOL_CAN_RAW, socket.CAN_RAW_RECV_OWN_MSGS, 0)
    return sock


def send(sock: socket.socket, can_id: int, data: bytes) -> None:
    sock.send(struct.pack(FRAME_FORMAT, can_id, len(data), data.ljust(8, b"\x00")))


def receive(sock: socket.socket) -> tuple[int, bytes]:
    can_id, dlc, data = struct.unpack(FRAME_FORMAT, sock.recv(FRAME_SIZE))
    return can_id & 0x7FF, data[:dlc]


def frames_of(vectors: dict) -> list[tuple[str, int, bytes]]:
    """Every (source, identifier, data) the vectors say a client sends or reads, in the order of the file."""
    frames: list[tuple[str, int, bytes]] = []
    for entry in vectors["identifiers"]:
        frames.append(("identifier", int(entry["id"], 16), bytes(range(8))))
    for entry in vectors["magic_payloads"]:
        frames.append((f"magic payload {entry['name']}", int(entry["id"], 16), bytes.fromhex(entry["data"])))
    for entry in vectors["slcan_encode"]:
        frames.append(("slcan encode", int(entry["id"], 16), bytes.fromhex(entry["data"])))
    for entry in vectors["slcan_decode"]:
        frame = entry["frame"]
        if frame is None:  # a line the reader must refuse: no frame
            continue
        frames.append((f"slcan decode {entry['name']}", int(frame["id"], 16), bytes.fromhex(frame["data"])))
    for entry in vectors["version_query"]:
        for can_id, data in entry["frames"]:
            frames.append((f"version query {entry['name']}", int(can_id, 16), bytes.fromhex(data)))
    return frames


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print("usage: vcan_urtc_vectors.py <interface>")
        return 2
    vectors = json.loads(VECTORS.read_text(encoding="utf-8"))
    failures: list[str] = []

    def expect(condition: bool, message: str) -> None:
        print(("PASS " if condition else "FAIL ") + message)
        if not condition:
            failures.append(message)

    expect(vectors.get("format") == 1 and len(vectors["identifiers"]) == 82, "the copy of the vectors is format 1 with its 82 identifiers")
    stack, head = open_socket(argv[1]), open_socket(argv[1])
    delivered = 0
    for source, can_id, data in frames_of(vectors):
        send(stack, can_id, data)
        try:
            got_id, got = receive(head)
        except socket.timeout:
            expect(False, f"{source} 0x{can_id:03X} was not delivered")
            continue
        delivered += 1
        if not (got_id == can_id and got == data):
            expect(False, f"{source} 0x{can_id:03X} arrived as 0x{got_id:03X} {got.hex()} instead of {data.hex()}")
    expect(delivered == len(frames_of(vectors)) and not failures, f"all {delivered} frames of the vectors cross the kernel unchanged")

    # a tool head answers a query in the order of the vectors: the frames of a version query come out in the order they went in
    for entry in vectors["version_query"]:
        sent = [(int(i, 16), bytes.fromhex(d)) for i, d in entry["frames"]]
        for can_id, data in sent:
            send(head, can_id, data)
        got = [receive(stack) for _ in sent]
        expect(got == sent, f"the version query '{entry['name']}' keeps its order")

    print(f"VCAN_URTC_VECTORS={'PASS' if not failures else 'FAIL'} ({len(failures)} failures)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
