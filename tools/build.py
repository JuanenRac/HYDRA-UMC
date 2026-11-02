#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/build.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""One entry point to build the firmware on Linux, macOS and Windows.

    python tools/build.py [--clean] [--ci] [--check] [--host-tests] [all|g474|h745]

* Without --check it runs the build for this platform (build_firmware.sh
  through bash, or build_firmware.bat on Windows) and returns its exit code.
* --ci sets HYDRA_UMC_CI=1 (no pauses, nothing is rewritten).
* --check builds nothing: it verifies that the two platform scripts pin the
  same third-party versions, and reports which tools are on the PATH.
  The exit code is non-zero when the pins drift apart.

* --host-tests builds nothing for a board: it compiles and runs the C host tests of the real firmware sources with the host compiler
  (tools/host_tests.py), the same on every platform.
* After a successful build the application images are signed when a signing seed is configured (HYDRA_FW_SIGNING_SEED_FILE, a file
  kept outside the repository; tools/sign_release.py) and the signatures are checked when HYDRA_FW_UPDATE_PUBKEY_HEX is set.
  HYDRA_UMC_RELEASE=1 makes a missing seed an error. Without a seed the images stay unsigned and the build says so.

Building never changes a version.
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SH = ROOT / "build_firmware.sh"
BAT = ROOT / "build_firmware.bat"
TARGETS = ("all", "g474", "h745")
TOOLS = ("arm-none-eabi-gcc", "git", "python3" if os.name != "nt" else "python")

_PIN_SH = re.compile(r'^([A-Z0-9_]+_(?:REPO|TAG))="([^"]*)"\s*$', re.MULTILINE)
_PIN_BAT = re.compile(r'^set "([A-Z0-9_]+_(?:REPO|TAG))=([^"]*)"\s*$', re.MULTILINE | re.IGNORECASE)


def read_pins(path: Path, pattern: re.Pattern[str]) -> dict[str, str]:
    """The pinned *_REPO / *_TAG values a platform script declares."""
    text = path.read_text(encoding="utf-8", errors="replace")
    return {name.upper(): value for name, value in pattern.findall(text)}


def pin_drift(sh_pins: dict[str, str], bat_pins: dict[str, str]) -> list[str]:
    """Human readable differences between the two scripts' pins."""
    problems: list[str] = []
    for name in sorted(set(sh_pins) | set(bat_pins)):
        if name not in sh_pins:
            problems.append(f"{name} is only pinned in {BAT.name}")
        elif name not in bat_pins:
            problems.append(f"{name} is only pinned in {SH.name}")
        elif sh_pins[name] != bat_pins[name]:
            problems.append(f"{name}: {SH.name} has {sh_pins[name]!r}, {BAT.name} has {bat_pins[name]!r}")
    if not sh_pins:
        problems.append(f"no pinned versions found in {SH.name}")
    return problems


def check() -> int:
    problems = pin_drift(read_pins(SH, _PIN_SH), read_pins(BAT, _PIN_BAT))
    for tool in TOOLS:
        print(f"  {'found  ' if shutil.which(tool) else 'missing'} {tool}")
    for problem in problems:
        print(f"DRIFT {problem}", file=sys.stderr)
    print("BUILD_CHECK=" + ("PASS" if not problems else "FAIL"))
    return 1 if problems else 0


def command_for(target: str, clean: bool, windows: bool) -> list[str]:
    """The platform script invocation for the requested target."""
    args = (["--clean"] if clean else []) + ([target] if target != "all" else [])
    if windows:
        return ["cmd", "/c", str(BAT), *args]
    return ["bash", str(SH), *args]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Build the HYDRA-UMC firmware on any platform.")
    parser.add_argument("target", nargs="?", default="all", choices=TARGETS)
    parser.add_argument("--clean", action="store_true", help="remove the build folder first")
    parser.add_argument("--ci", action="store_true", help="non-interactive build (HYDRA_UMC_CI=1)")
    parser.add_argument("--check", action="store_true", help="verify the scripts and tools, build nothing")
    parser.add_argument("--host-tests", action="store_true", help="compile and run the C host tests, build nothing for a board")
    options = parser.parse_args(argv)
    if options.check:
        return check()
    if options.host_tests:
        return subprocess.call([sys.executable, str(ROOT / "tools" / "host_tests.py")], cwd=ROOT)
    env = dict(os.environ)
    if options.ci:
        env["HYDRA_UMC_CI"] = "1"
    command = command_for(options.target, options.clean, os.name == "nt")
    print("+", " ".join(command))
    status = subprocess.call(command, cwd=ROOT, env=env)
    if status != 0:
        return status
    return sign_and_verify(env)


def sign_and_verify(env: dict[str, str]) -> int:
    """Sign the application images with the seed kept outside the repository, then check the signatures against the public key."""
    sign_tool = str(ROOT / "tools" / "sign_release.py")
    status = subprocess.call([sys.executable, sign_tool, "sign"], cwd=ROOT, env=env)
    if status != 0 or not env.get("HYDRA_FW_UPDATE_PUBKEY_HEX"):
        return status
    return subprocess.call([sys.executable, sign_tool, "verify"], cwd=ROOT, env=env)


if __name__ == "__main__":
    raise SystemExit(main())
