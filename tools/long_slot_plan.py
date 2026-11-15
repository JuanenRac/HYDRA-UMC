#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/long_slot_plan.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The long-slot variant of the CM7 firmware of the H757 board, as data and as the checks of its build.

    python tools/long_slot_plan.py --check                       the sources, the linker script, the profile and config/long_slot.cfg say the same thing
    python tools/long_slot_plan.py --plan                        print the definitions and the files of the variant
    python tools/long_slot_plan.py --link BOOT.elf APP.elf       the sizes of the two linked images against their regions and the interface budget

The variant is the CM7 bootloader compiled with the 768 KB slot (HYDRA_APP_SLOT_768K, which needs the copy in the NOR: HYDRA_NOR_BACKUP and HYDRA_NOR_SPI5) and the CM7 application linked
with the 768 KB script. It is a build option that ships OFF (HYDRA_LONG_SLOT=1 in the environment of tools/build_firmware.py, step 10c): without it the build is the one it was, and the long
images are left in build/h745/<output_dir>, never in firmware/. The link check is where the budget of the interface library is compared with a linked firmware: the 300 KiB that
docs/UI_STACK_H757.md counts are for the linked image, so the size of the application that was just linked is added to the interface of tools/ui_budget.py and the sum must leave the configured share
of the slot free. Evidence: simulator (the sizes are those of a build, nothing ran on a board)."""
from __future__ import annotations

import argparse
import configparser
import json
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bootloader_size  # noqa: E402
import ui_budget  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
CFG_PATH = ROOT / "config" / "long_slot.cfg"
BOOT_COMMON = "src/mcu_stm32h757/CM7/boot/bootloader_common.h"
BOOT_LINKER = "src/mcu_stm32h757/CM7/boot/STM32H757XIHx_CM7_BOOTLOADER.ld"
REQUIRED_DEFINES = ("HYDRA_APP_SLOT_768K=1", "HYDRA_NOR_BACKUP=1", "HYDRA_NOR_SPI5=1")
SUFFIX_RE = re.compile(r"^[A-Z0-9_]{1,24}$")
DIR_RE = re.compile(r"^[a-z0-9_]{1,40}$")
DEFINE_RE = re.compile(r"^HYDRA_[A-Z0-9_]+(=[A-Za-z0-9_]+)?$")


def load_config(path: Path = CFG_PATH) -> dict:
    """The values of config/long_slot.cfg, refused when one is out of range or has characters that a name or a definition does not have."""
    parser = configparser.ConfigParser(inline_comment_prefixes=None)
    parser.read_string(path.read_text(encoding="utf-8"))
    cfg = {
        "output_dir": parser["variant"]["output_dir"].strip(),
        "image_suffix": parser["variant"]["image_suffix"].strip(),
        "bootloader_defines": parser["bootloader"]["defines"].split(),
        "linker": parser["application"]["linker"].strip(),
        "free_percent": float(parser["budget"]["free_percent"]),
    }
    if not DIR_RE.fullmatch(cfg["output_dir"]):
        raise ValueError("output_dir is not a plain folder name")
    if not SUFFIX_RE.fullmatch(cfg["image_suffix"]):
        raise ValueError("image_suffix is not a name of capital letters, digits and underscores")
    for define in cfg["bootloader_defines"]:
        if not DEFINE_RE.fullmatch(define):
            raise ValueError(f"not a definition of the project: {define!r}")
    for required in REQUIRED_DEFINES:
        if required not in cfg["bootloader_defines"]:
            raise ValueError(f"the long slot needs {required}")
    if not 0 <= cfg["free_percent"] < 100:
        raise ValueError("free_percent must be at least 0 and less than 100")
    return cfg


def compile_flags(cfg: dict) -> list[str]:
    """The -D options of the bootloader of the variant."""
    return [f"-D{define}" for define in cfg["bootloader_defines"]]


def image_name(component: str, version_tag: str, cfg: dict) -> str:
    """The name of a long-slot image: it carries the suffix, so the sums of the default set (h745_invariance.py) never see it."""
    return f"HYDRA_KB_CM7_{component}_{cfg['image_suffix']}_{version_tag}"


def slot_bytes(root: Path = ROOT, cfg: dict | None = None) -> int:
    """The length of FLASH_APP in the linker script of the long slot."""
    cfg = cfg or load_config()
    found = bootloader_size.regions((root / cfg["linker"]).read_text(encoding="utf-8"))
    if "FLASH_APP" not in found:
        raise KeyError(f"{cfg['linker']} has no FLASH_APP region")
    return found["FLASH_APP"]


def link_check(boot: dict, app_flash_used: int, slot: int, ui_flash: int, free_percent: float, boot_margin: int) -> tuple[bool, list[str]]:
    """The verdict on two linked images. `boot` is a row of bootloader_size.measure (flash_free, flash_region); the application and the interface must leave
    `free_percent` of the slot free. Returns the verdict and the lines that say why."""
    lines: list[str] = []
    ok = True
    if boot["flash_free"] < boot_margin:
        ok = False
        lines.append(f"FAIL the bootloader leaves {boot['flash_free']} B free in its {boot['flash_region']} B, below the margin of {boot_margin} B")
    else:
        lines.append(f"OK   the bootloader leaves {boot['flash_free']} B free in its {boot['flash_region']} B (margin {boot_margin} B)")
    need = app_flash_used + ui_flash
    limit = int(slot * (1 - free_percent / 100))
    kib = lambda value: f"{value / 1024:.1f} KiB"  # noqa: E731
    verdict = "fits" if need <= limit else "DOES NOT FIT"
    lines.append(f"{'OK  ' if need <= limit else 'FAIL'} application {kib(app_flash_used)} + interface {kib(ui_flash)} = {kib(need)} against {kib(limit)} ({100 - free_percent:g} % of the slot of {kib(slot)}): {verdict}")
    if need > limit:
        ok = False
    return ok, lines


def read_size(elf: Path, size_tool: str = "arm-none-eabi-size") -> tuple[int, int, int]:
    return bootloader_size.parse_size(subprocess.run([size_tool, str(elf)], check=True, capture_output=True, text=True).stdout)


def check_sources(root: Path = ROOT) -> list[str]:
    """What the sources, the linker script, the profile and the file must agree on; the problems found (empty when all is well)."""
    problems: list[str] = []
    cfg = load_config(root / "config" / "long_slot.cfg")
    common = (root / BOOT_COMMON).read_text(encoding="utf-8")
    if "#define APP_MAX_SIZE         (768UL * 1024UL)" not in common:
        problems.append("the bootloader has no 768 KB APP_MAX_SIZE behind HYDRA_APP_SLOT_768K")
    if "#define APP_MAX_SIZE         (384UL * 1024UL)" not in common:
        problems.append("the default APP_MAX_SIZE of the bootloader is no longer 384 KB: the default images would change")
    if "#error \"HYDRA_APP_SLOT_768K needs HYDRA_NOR_BACKUP=1" not in common:
        problems.append("the long slot no longer refuses to build without the copy in the NOR")
    slot = slot_bytes(root, cfg)
    if slot != 768 * 1024:
        problems.append(f"the linker script of the long slot has {slot} B, not 768 KiB")
    nor = configparser.ConfigParser(inline_comment_prefixes=None)
    nor.read_string((root / "config" / "nor_backup.cfg").read_text(encoding="utf-8"))
    if int(nor["slot"]["app_bytes"]) != slot:
        problems.append("config/nor_backup.cfg protects another slot size than the linker script of the long slot")
    default_script = bootloader_size.regions((root / "src/mcu_stm32h757/CM7/STM32H757XIHx_CM7_APP.ld").read_text(encoding="utf-8"))
    if default_script.get("FLASH_APP") != 384 * 1024:
        problems.append("the default CM7 application script is no longer 384 KB")
    profile_slot = ui_budget.regions()["FLASH_APP_CM7"]["length"]
    if profile_slot != slot:
        problems.append(f"the profile h757_target has {profile_slot} B for the CM7 application, the long-slot script {slot} B")
    entry = json.loads((root / "tools" / "bootloader_size.json").read_text(encoding="utf-8"))["bootloaders"]["kb_cm7"]
    if entry["linker"] != BOOT_LINKER:
        problems.append("tools/bootloader_size.json measures another linker script for the CM7 bootloader")
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="the sources, the script, the profile and the file agree")
    parser.add_argument("--plan", action="store_true", help="print the definitions and the names of the variant")
    parser.add_argument("--link", nargs=2, metavar=("BOOT_ELF", "APP_ELF"), help="check the sizes of the two linked images")
    parser.add_argument("--root", default=str(ROOT), help="the repository (default: the one this script is in)")
    args = parser.parse_args(argv)
    root = Path(args.root)
    cfg = load_config(root / "config" / "long_slot.cfg")
    if args.check:
        problems = check_sources(root)
        for problem in problems:
            print(f"FAIL {problem}")
        print("long_slot_plan:", "FAIL" if problems else "PASS")
        return 1 if problems else 0
    if args.plan:
        print("bootloader flags:", " ".join(compile_flags(cfg)))
        print("application linker:", cfg["linker"])
        print("folder:", f"build/h745/{cfg['output_dir']}")
        print("names:", image_name("BOOTLOADER", "vX.Y.Z", cfg), image_name("APP", "vX.Y.Z", cfg))
        return 0
    if args.link:
        boot_elf, app_elf = (Path(p) for p in args.link)
        text, data, _bss = read_size(app_elf)
        boot_text, boot_data, boot_bss = read_size(boot_elf)
        entry = json.loads((root / "tools" / "bootloader_size.json").read_text(encoding="utf-8"))["bootloaders"]["kb_cm7"]
        region = bootloader_size.regions((root / entry["linker"]).read_text(encoding="utf-8"))[entry["flash_region"]]
        boot = {"flash_free": region - (boot_text + boot_data), "flash_region": region, "bss": boot_bss}
        ok, lines = link_check(boot, text + data, slot_bytes(root, cfg), ui_budget.compute()["flash_ui_bytes"], cfg["free_percent"], int(entry["flash_margin_bytes"]))
        for line in lines:
            print(line)
        print("long_slot_plan:", "PASS" if ok else "FAIL")
        return 0 if ok else 1
    parser.print_help()
    return 0


if __name__ == "__main__":
    sys.exit(main())
