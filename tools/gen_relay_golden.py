#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/gen_relay_golden.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Writes tests/relay_golden_vectors.json: what the real relay logic of the Robot Controller Board (src/mcu_stm32g474/relay_ids.h,
compiled for the PC through tests/relay_golden_dump.c) answers, so that the Python fakes and the CM5 relay code are checked against bytes
the firmware made instead of against a hand-written copy of its rules.

    python tools/gen_relay_golden.py          compiles the firmware header for the PC and writes the file
    python tools/gen_relay_golden.py --check  exits 1 when the file is not what the firmware makes (0 and a notice without a compiler)
"""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import host_tests  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
OUTPUT = ROOT / "tests" / "relay_golden_vectors.json"
SOURCE = ROOT / "tests" / "relay_golden_dump.c"


def firmware_output(compiler: str) -> str:
    """The dump of the firmware logic compiled for the PC, as normalised JSON text."""
    with tempfile.TemporaryDirectory() as folder:
        binary = Path(folder) / ("dump.exe" if sys.platform == "win32" else "dump")
        command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", f"-I{ROOT / 'src' / 'mcu_stm32g474'}", "-o", str(binary), str(SOURCE)]
        subprocess.run(command, check=True)
        text = subprocess.run([str(binary)], check=True, capture_output=True, text=True).stdout
    return json.dumps(json.loads(text), indent=1) + "\n"


def main(argv: list[str]) -> int:
    compiler = host_tests.find_compiler()
    if compiler is None:
        print("RELAY_GOLDEN=SKIPPED no host C compiler (cc/gcc/clang) found; set HOST_CC")
        return 0
    text = firmware_output(compiler)
    if "--check" in argv:
        current = OUTPUT.read_text(encoding="utf-8") if OUTPUT.is_file() else ""
        if current.replace("\r\n", "\n") != text:
            print("RELAY_GOLDEN=FAIL tests/relay_golden_vectors.json is not what the firmware makes: run python tools/gen_relay_golden.py")
            return 1
        print("RELAY_GOLDEN=PASS")
        return 0
    OUTPUT.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {OUTPUT.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
