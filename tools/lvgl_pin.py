# =============================================================================
# HYDRA-UMC - tools/lvgl_pin.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The interface library of the H757 board, pinned by commit (config/lvgl.cfg). The library is not part of this repository: this tool checks the pin, fetches that exact commit into
an empty folder OUTSIDE the repository (Actions does it, .github/workflows/lvgl-library.yml) and compiles the library for the Cortex-M7 with the settings of config/lv_conf.h, then
checks what it takes of the flash against the budget of the file. Evidence: simulator (the compile is a build, nothing runs).

    python tools/lvgl_pin.py --check
    python tools/lvgl_pin.py --fetch DIR
    python tools/lvgl_pin.py --compile DIR OUT
"""
from __future__ import annotations

import argparse
import configparser
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CFG_PATH = ROOT / "config" / "lvgl.cfg"
SHA_RE = re.compile(r"^[0-9a-f]{40}$")
TAG_RE = re.compile(r"^v9\.\d+\.\d+$")
REPOSITORY = "https://github.com/lvgl/lvgl"
CORE_FLAGS = ["-mcpu=cortex-m7", "-mthumb", "-mfpu=fpv5-d16", "-mfloat-abi=hard", "-Os", "-ffunction-sections", "-fdata-sections", "-Wno-unused-parameter"]
VENDORED_NAMES = {"lvgl.h", "lv_version.h", "lv_conf_internal.h", "lv_obj.c", "lv_timer.c"}


def load_config(path: Path = CFG_PATH) -> dict:
    parser = configparser.ConfigParser(inline_comment_prefixes=None)
    parser.read_string(path.read_text(encoding="utf-8"))
    library = parser["library"]
    cfg = {
        "repository": library["repository"].strip(),
        "tag": library["tag"].strip(),
        "commit": library["commit"].strip(),
        "license": library["license"].strip(),
        "flash_budget_bytes": int(library["flash_budget_bytes"]),
    }
    check(cfg)
    return cfg


def check(cfg: dict) -> None:
    """ValueError when the pin is not an exact commit of the project, the tag is not a 9.x release, or the budget is not a size."""
    if cfg["repository"] != REPOSITORY:
        raise ValueError("config/lvgl.cfg: the repository must be " + REPOSITORY)
    if not SHA_RE.match(cfg["commit"]):
        raise ValueError("config/lvgl.cfg: the commit must be 40 lowercase hexadecimal digits (an exact commit, never a branch or a tag)")
    if not TAG_RE.match(cfg["tag"]):
        raise ValueError("config/lvgl.cfg: the tag must be a release of the 9.x series, like v9.6.0")
    if cfg["license"] != "MIT":
        raise ValueError("config/lvgl.cfg: the license of the pinned library must be MIT")
    if not 16384 <= cfg["flash_budget_bytes"] <= 786432:
        raise ValueError("config/lvgl.cfg: flash_budget_bytes outside 16384..786432")


def vendored_files(root: Path = ROOT) -> list[str]:
    """The files of the library that someone copied into the repository; there must be none."""
    skip = {".git", "build", "node_modules", "__pycache__"}
    found = []
    for path in root.rglob("*"):
        if any(part in skip for part in path.relative_to(root).parts):
            continue
        if path.is_file() and path.name in VENDORED_NAMES:
            found.append(path.relative_to(root).as_posix())
    return sorted(found)


def fetch(cfg: dict, dest: Path, run=subprocess.run, root: Path = ROOT) -> str:
    """Fetches the pinned commit into `dest` (an empty folder outside the repository) and returns the commit that was checked out; RuntimeError when it is not the pin."""
    check(cfg)
    dest = Path(dest).resolve()
    if dest == root.resolve() or root.resolve() in dest.parents:
        raise ValueError("the library is fetched outside the repository")
    if dest.exists() and any(dest.iterdir()):
        raise ValueError("the destination folder must be empty")
    dest.mkdir(parents=True, exist_ok=True)
    for args in (["init", "-q"], ["fetch", "-q", "--depth", "1", cfg["repository"], cfg["commit"]], ["checkout", "-q", "--detach", "FETCH_HEAD"]):
        run(["git", "-C", str(dest), *args], check=True)
    head = run(["git", "-C", str(dest), "rev-parse", "HEAD"], check=True, capture_output=True, text=True).stdout.strip()
    if head != cfg["commit"]:
        raise RuntimeError(f"the checked out commit {head} is not the pin {cfg['commit']}")
    return head


def sources(library: Path) -> list[Path]:
    return sorted((library / "src").rglob("*.c"))


def parse_size(text: str) -> dict:
    """The totals of `arm-none-eabi-size` (Berkeley format) over several objects: text, data and bss in bytes."""
    total = {"text": 0, "data": 0, "bss": 0}
    for line in text.splitlines():
        parts = line.split()
        if len(parts) >= 4 and all(p.isdigit() for p in parts[:3]):
            total["text"] += int(parts[0])
            total["data"] += int(parts[1])
            total["bss"] += int(parts[2])
    return total


def flash_bytes(sizes: dict) -> int:
    """What the library takes of the flash: the code and constants plus the initialised data."""
    return sizes["text"] + sizes["data"]


def compile_library(cfg: dict, library: Path, out: Path, run=subprocess.run, root: Path = ROOT) -> dict:
    """Compiles every source of the library for the Cortex-M7 and returns the sizes; RuntimeError when the library takes more flash than the budget."""
    check(cfg)
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    objects = []
    for index, source in enumerate(sources(library)):
        obj = out / f"{index:04d}_{source.stem}.o"
        run(
            ["arm-none-eabi-gcc", *CORE_FLAGS, "-DLV_CONF_INCLUDE_SIMPLE", f"-I{root / 'config'}", f"-I{library}", f"-I{library / 'src'}", "-c", str(source), "-o", str(obj)],
            check=True,
        )
        objects.append(str(obj))
    if not objects:
        raise RuntimeError("the library has no source")
    sizes = parse_size(run(["arm-none-eabi-size", *objects], check=True, capture_output=True, text=True).stdout)
    sizes["objects"] = len(objects)
    sizes["flash"] = flash_bytes(sizes)
    if sizes["flash"] > cfg["flash_budget_bytes"]:
        raise RuntimeError(f"the library takes {sizes['flash']} bytes of flash, more than the budget of {cfg['flash_budget_bytes']}")
    return sizes


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="check the pin and that no file of the library is in the repository")
    parser.add_argument("--fetch", metavar="DIR", help="fetch the pinned commit into an empty folder outside the repository")
    parser.add_argument("--compile", nargs=2, metavar=("DIR", "OUT"), help="compile the library fetched in DIR for the Cortex-M7 into OUT and check the flash budget")
    args = parser.parse_args(argv)
    cfg = load_config()
    if args.check:
        found = vendored_files()
        if found:
            print("LVGL_PIN=FAIL files of the library are in the repository: " + ", ".join(found))
            return 1
        print(f"LVGL_PIN=PASS {cfg['tag']} {cfg['commit']}")
    if args.fetch:
        print("LVGL_FETCH=PASS " + fetch(cfg, Path(args.fetch)))
    if args.compile:
        sizes = compile_library(cfg, Path(args.compile[0]), Path(args.compile[1]))
        print(f"LVGL_COMPILE=PASS objects={sizes['objects']} flash={sizes['flash']} bss={sizes['bss']} budget={cfg['flash_budget_bytes']}")
    if not (args.check or args.fetch or args.compile):
        parser.print_help()
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
