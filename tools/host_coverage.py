#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/host_coverage.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Line coverage of the firmware modules that the C host tests exercise, one figure per module.

    python tools/host_coverage.py [--check] [--json OUT] [--missing]

Every test of tools/host_tests.py is built with gcc coverage, run, and read with gcov. The lines of one module that ran in ANY flavour of the
tests (switches off, switches on, each core) count as covered, so a module that is compiled in several flavours is not penalised for the
code a flavour leaves out. A figure is kept for each module on its own: there is no global number that could hide a bare module. With
--check the run fails when a module falls below its own floor in tools/host_coverage_floor.json. It compiles, so it only runs where a C
compiler and gcov exist and local builds are allowed (a CI runner, or ALLOW_LOCAL_BUILD=1); anywhere else it says it was skipped.
"""
from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
import host_tests  # noqa: E402
from local_build import local_build_refusal  # noqa: E402

FLOOR_FILE = ROOT / "tools" / "host_coverage_floor.json"
WORK = ROOT / "build" / "host_coverage"


def module_floors(path: Path = FLOOR_FILE) -> dict[str, float]:
    """The floors of the modules: {source path relative to the repository: minimum percentage of lines}."""
    data = json.loads(path.read_text(encoding="utf-8"))
    return {str(key): float(value) for key, value in data["modules"].items()}


def merge_counts(per_run: list[dict[str, dict[int, int]]]) -> dict[str, dict[int, int]]:
    """Merge the line counts of several runs: a line is the sum of its counts, a line seen in only one run stays."""
    merged: dict[str, dict[int, int]] = {}
    for run in per_run:
        for name, lines in run.items():
            target = merged.setdefault(name, {})
            for number, count in lines.items():
                target[number] = target.get(number, 0) + count
    return merged


def percent(lines: dict[int, int]) -> float:
    """The share of the instrumented lines that ran, 100.0 for a module with no code line."""
    if not lines:
        return 100.0
    return 100.0 * sum(1 for count in lines.values() if count > 0) / len(lines)


def parse_gcov_json(text: str, root: Path = ROOT) -> dict[str, dict[int, int]]:
    """{file relative to the repository: {line: count}} from the JSON that gcov writes; files outside the repository are left out."""
    result: dict[str, dict[int, int]] = {}
    document = json.loads(text)
    base = Path(document.get("current_working_directory", root))
    for entry in document.get("files", []):
        path = Path(entry["file"])
        if not path.is_absolute():
            path = base / path
        try:
            relative = path.resolve().relative_to(root.resolve()).as_posix()
        except ValueError:
            continue
        lines = result.setdefault(relative, {})
        for line in entry.get("lines", []):
            number = int(line["line_number"])
            lines[number] = lines.get(number, 0) + int(line["count"])
    return result


def judge(figures: dict[str, float], floors: dict[str, float]) -> list[str]:
    """The modules that fall below their floor, or that have no figure at all."""
    problems = []
    for module, floor in sorted(floors.items()):
        if module not in figures:
            problems.append(f"{module}: no figure (the tests did not reach it)")
        elif figures[module] + 1e-9 < floor:
            problems.append(f"{module}: {figures[module]:.1f}% is below its floor of {floor:.1f}%")
    return problems


def measure(compiler: str) -> dict[str, dict[int, int]]:
    """Build every test with coverage, run it and read the counters with gcov; the counts of all runs, merged."""
    if WORK.exists():
        shutil.rmtree(WORK)
    WORK.mkdir(parents=True)
    tweetnacl = WORK / "tweetnacl.o"
    subprocess.run([compiler, "-std=gnu11", "-w", "-c", str(ROOT / host_tests.G4 / "boot" / "tweetnacl.c"), "-o", str(tweetnacl)], check=True)
    runs: list[dict[str, dict[int, int]]] = []
    for test in host_tests.table():
        if test.syntax_only:
            continue
        folder = WORK / host_tests._safe(test.name)
        folder.mkdir()
        binary = folder / "t"
        command = host_tests.build_command(compiler, test, binary, tweetnacl)
        command[1:1] = ["--coverage", "-O0", "-g", "-fkeep-inline-functions", "-fkeep-static-functions"]  # a function nobody calls must show as 0%, not vanish
        if subprocess.run(command, cwd=folder).returncode != 0:
            raise SystemExit(f"{test.name}: does not compile with coverage")
        if subprocess.run([str(binary)], cwd=folder, stdout=subprocess.DEVNULL).returncode != 0:
            raise SystemExit(f"{test.name}: fails under coverage")
        for gcda in folder.glob("*.gcda"):
            done = subprocess.run(["gcov", "--json-format", "--stdout", str(gcda)], cwd=folder, capture_output=True, text=True)
            if done.returncode != 0:
                raise SystemExit(f"{test.name}: gcov failed: {done.stderr[:200]}")
            for chunk in done.stdout.splitlines():
                if chunk.strip().startswith("{"):
                    runs.append(parse_gcov_json(chunk, ROOT))
    return merge_counts(runs)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Line coverage per firmware module of the C host tests.")
    parser.add_argument("--check", action="store_true", help="fail when a module is below its floor")
    parser.add_argument("--missing", action="store_true", help="list the lines that no test ran")
    parser.add_argument("--json", metavar="OUT", help="write the figures to a file")
    options = parser.parse_args(argv)
    refusal = local_build_refusal()
    compiler = host_tests.find_compiler()
    if refusal is not None or compiler is None or shutil.which("gcov") is None:
        print("HOST_COVERAGE=SKIPPED " + (refusal or "no host C compiler or no gcov"))
        return 0
    counts = measure(compiler)
    floors = module_floors()
    figures = {module: percent({n: c for n, c in counts.get(module, {}).items()}) for module in floors if module in counts}
    for module in sorted(floors):
        shown = f"{figures[module]:6.1f}%" if module in figures else "   none"
        print(f"{shown}  (floor {floors[module]:5.1f}%)  {module}")
        if options.missing and module in counts:
            gaps = sorted(number for number, count in counts[module].items() if count == 0)
            if gaps:
                print("          not run: " + ",".join(str(number) for number in gaps))
    if options.missing:
        for module in sorted(set(counts) - set(floors)):
            if module.startswith("src/"):
                print(f"{percent(counts[module]):6.1f}%  (no floor)         {module}")
                gaps = sorted(number for number, count in counts[module].items() if count == 0)
                if gaps:
                    print("          not run: " + ",".join(str(number) for number in gaps))
    if options.json:
        Path(options.json).write_text(json.dumps(figures, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    problems = judge(figures, floors)
    for problem in problems:
        print("LOW " + problem)
    if options.check and problems:
        print("HOST_COVERAGE=FAIL")
        return 1
    print("HOST_COVERAGE=PASS" if not problems else "HOST_COVERAGE=WARN")
    return 0


if __name__ == "__main__":
    sys.exit(main())
