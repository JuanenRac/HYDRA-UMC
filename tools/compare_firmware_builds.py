#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/compare_firmware_builds.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
#
# Builds the firmware twice, once with build_firmware.sh and once with tools/build_firmware.py, in the same folder one after the other (so that no
# path in a binary can differ), and compares the SHA-256 of every file that each leaves in firmware/. It needs arm-none-eabi-gcc and the network (the
# vendor sources are fetched), so it runs in the workflow `firmware-build-compare`, not in the tests. The locale is C for both, so the objects are
# linked in the same order.
#
#   python3 tools/compare_firmware_builds.py [--keep]
# =============================================================================
from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SKIP_COPY = {".git", "build", "firmware", "__pycache__"}


def sha256_tree(folder: Path) -> dict[str, str]:
    """The SHA-256 of every file under `folder`, by path relative to it."""
    sums: dict[str, str] = {}
    for path in sorted(folder.rglob("*")):
        if path.is_file():
            sums[path.relative_to(folder).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    return sums


def differences(shell: dict[str, str], python: dict[str, str]) -> list[str]:
    """What differs between two listings, one line each."""
    lines = []
    for name in sorted(set(shell) | set(python)):
        if name not in python:
            lines.append(f"only the shell script made {name}")
        elif name not in shell:
            lines.append(f"only the Python script made {name}")
        elif shell[name] != python[name]:
            lines.append(f"{name} differs: shell {shell[name][:16]}..., python {python[name][:16]}...")
    return lines


def run_build(work: Path, command: list[str]) -> int:
    env = dict(os.environ, LC_ALL="C", HYDRA_UMC_CI="1")
    env.pop("HYDRA_UMC_RELEASE", None)
    env.pop("HYDRA_EXTRA_CFLAGS", None)
    return subprocess.run(command, cwd=work, env=env, check=False, stdin=subprocess.DEVNULL).returncode


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Build the firmware with both scripts and compare what they leave in firmware/.")
    parser.add_argument("--keep", action="store_true", help="keep the working folder")
    args = parser.parse_args(argv)
    work = Path(tempfile.mkdtemp(prefix="hydra-firmware-compare-"))
    try:
        shutil.copytree(ROOT, work / "repo", ignore=lambda _folder, names: [n for n in names if n in SKIP_COPY])
        repo = work / "repo"
        print("== build_firmware.sh ==", flush=True)
        if run_build(repo, ["bash", "build_firmware.sh"]) != 0:
            print("COMPARE=FAIL the shell script failed")
            return 1
        shell = sha256_tree(repo / "firmware")
        shutil.rmtree(repo / "build")
        shutil.rmtree(repo / "firmware")
        print("== tools/build_firmware.py ==", flush=True)
        if run_build(repo, [sys.executable, "tools/build_firmware.py"]) != 0:
            print("COMPARE=FAIL the Python script failed")
            return 1
        python = sha256_tree(repo / "firmware")
        found = differences(shell, python)
        print(f"{len(shell)} files from the shell script, {len(python)} from the Python script")
        for name, digest in sorted(python.items()):
            print(f"  {digest}  {name}")
        if not shell:
            print("COMPARE=FAIL the shell script left nothing in firmware/")
            return 1
        if found:
            print("\n".join(found))
            print("COMPARE=FAIL the two builds do not leave the same files")
            return 1
        print("COMPARE=PASS the two builds leave the same files, byte for byte")
        return 0
    finally:
        if args.keep:
            print(f"kept {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
