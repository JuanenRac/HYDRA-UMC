# =============================================================================
# HYDRA-UMC - python -m spi_bridge: starts the local HTTP service and, when asked, the publisher of the CAN error counters
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see repo root LICENSE
# =============================================================================
"""`python -m spi_bridge serve --spi-device /dev/spidev0.0 --gpio-chip /dev/gpiochip4 --gpio-line 17 --hmac-key-file FILE`

The HMAC key of the firmware images comes from a file (32 bytes, raw or 64 hexadecimal characters), never from the command line. With
`--collector-url` and at least one `--counter-target tier:slot[:relay]` the service also reads the error counters of those boards every
`--counter-interval` seconds and sends them to HYDRA-UMC-TELEMETRY-COLLECTOR; the collector token comes from `--collector-token-file`
or `HYDRA_UMC_SPI_BRIDGE_COLLECTOR_TOKEN`.
"""

from __future__ import annotations

import argparse
import os
import sys
import threading
from pathlib import Path

from .http_service import serve
from .telemetry_publisher import CounterPublisher, parse_target
from .transport import open_spi_transport

COLLECTOR_TOKEN_ENV = "HYDRA_UMC_SPI_BRIDGE_COLLECTOR_TOKEN"


def read_hmac_key(path: str) -> bytes:
    raw = Path(path).read_bytes().strip()
    if len(raw) == 64:
        try:
            return bytes.fromhex(raw.decode("ascii"))
        except (ValueError, UnicodeDecodeError):
            pass
    if len(raw) == 32:
        return raw
    raise ValueError("the HMAC key file must hold 32 bytes or 64 hexadecimal characters")


def read_token(path: str | None, environ: dict[str, str]) -> str | None:
    if path:
        token = Path(path).read_text(encoding="utf-8").strip()
    else:
        token = environ.get(COLLECTOR_TOKEN_ENV, "").strip()
    return token or None


def build_publisher(args: argparse.Namespace, transport, bus_lock: threading.Lock, environ: dict[str, str]) -> CounterPublisher | None:
    """The publisher the arguments ask for, or None when they ask for none."""

    if not args.collector_url:
        if args.counter_target:
            raise ValueError("--counter-target needs --collector-url")
        return None
    if not args.counter_target:
        raise ValueError("--collector-url needs at least one --counter-target")
    targets = [parse_target(text) for text in args.counter_target]
    return CounterPublisher(
        transport,
        bus_lock,
        targets,
        args.collector_url,
        token=read_token(args.collector_token_file, environ),
        interval_seconds=args.counter_interval,
    )


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="python -m spi_bridge")
    commands = parser.add_subparsers(dest="command", required=True)
    run = commands.add_parser("serve", help="start the local HTTP service")
    run.add_argument("--spi-device", default="/dev/spidev0.0")
    run.add_argument("--gpio-chip", required=True)
    run.add_argument("--gpio-line", type=int, required=True)
    run.add_argument("--spi-speed-hz", type=int, default=10_000_000)
    run.add_argument("--hmac-key-file", required=True)
    run.add_argument("--host", default="127.0.0.1")
    run.add_argument("--port", type=int, default=8765)
    run.add_argument("--collector-url", help="HYDRA-UMC-TELEMETRY-COLLECTOR base address, for example http://127.0.0.1:8092")
    run.add_argument("--collector-token-file")
    run.add_argument("--counter-target", action="append", default=[], help="tier:slot or tier:slot:relay; repeat for each board")
    run.add_argument("--counter-interval", type=float, default=10.0)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = make_parser().parse_args(argv)
    try:
        key = read_hmac_key(args.hmac_key_file)
        transport = open_spi_transport(args.spi_device, args.gpio_chip, args.gpio_line, spi_speed_hz=args.spi_speed_hz)
        bus_lock = threading.Lock()
        publisher = build_publisher(args, transport, bus_lock, dict(os.environ))
    except (ValueError, OSError, RuntimeError) as error:
        print(f"[spi-bridge] {error}", file=sys.stderr)
        return 2
    server = serve(transport, key, host=args.host, port=args.port, bus_lock=bus_lock)
    stop = None
    if publisher is not None:
        _, stop = publisher.start()
        print(f"[spi-bridge] sending the CAN error counters of {len(args.counter_target)} board(s) to {args.collector_url}")
    print(f"[spi-bridge] HTTP service on {args.host}:{args.port}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        if stop is not None:
            stop.set()
        server.server_close()
        transport.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
