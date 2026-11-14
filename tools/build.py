#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/build.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""One entry point to build the firmware on Linux, macOS and Windows.

    python tools/build.py [--clean] [--ci] [--check] [--host-tests] [all|g474|h745]

* Without --check it runs the build (tools/build_firmware.py, the same script on every platform) and returns its exit code.
* --ci sets HYDRA_UMC_CI=1 (no pauses, nothing is rewritten).
* --check builds nothing: it verifies that the build script pins the
  pins are all declared, and reports which tools are on the PATH.
  The exit code is non-zero when a pin is missing.

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
SCRIPT = ROOT / "tools" / "build_firmware.py"
TARGETS = ("all", "g474", "h745")
TOOLS = ("arm-none-eabi-gcc", "git", "python3" if os.name != "nt" else "python")

_PIN = re.compile(r'^([A-Z0-9_]+_(?:REPO|TAG)) = "([^"]*)"\s*$', re.MULTILINE)


def read_pins(path: Path) -> dict[str, str]:
    """The pinned *_REPO / *_TAG values the build script declares."""
    return dict(_PIN.findall(path.read_text(encoding="utf-8", errors="replace")))


def pin_problems(pins: dict[str, str]) -> list[str]:
    """What is wrong with the pins: none found, or a repository without its tag (or the other way round)."""
    problems: list[str] = []
    if not pins:
        problems.append(f"no pinned versions found in {SCRIPT.name}")
    for name in sorted(pins):
        other = name[:-5] + "_TAG" if name.endswith("_REPO") else name[:-4] + "_REPO"
        if other not in pins:
            problems.append(f"{name} has no {other} in {SCRIPT.name}")
    return problems


def check() -> int:
    problems = pin_problems(read_pins(SCRIPT))
    for tool in TOOLS:
        print(f"  {'found  ' if shutil.which(tool) else 'missing'} {tool}")
    for problem in problems:
        print(f"PIN {problem}", file=sys.stderr)
    print("BUILD_CHECK=" + ("PASS" if not problems else "FAIL"))
    return 1 if problems else 0


def command_for(target: str, clean: bool) -> list[str]:
    """The invocation of the build script for the requested target."""
    args = (["--clean"] if clean else []) + ([target] if target != "all" else [])
    return [sys.executable, str(SCRIPT), *args]


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
    command = command_for(options.target, options.clean)
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
