#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/build_firmware.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
#
# The firmware build as one script that runs the same everywhere, to replace build_firmware.sh and build_firmware.bat. It is ported one stage at a
# time and checked against the shell script at every stage (tests/test_build_firmware_port.py runs both with stand-in tools and compares the
# commands they run, the lines they print and the files they leave):
#
#   stage 1   the toolchain check and the logic tests of the host (plain gcc)                    ported
#   stage 2   the compilation and the linking of the three boards (steps 2 to 10b)               ported
#   stage 3+  the move into firmware/ and the manifest                                           not ported yet: use build_firmware.sh
#
# Building does not change any version. Raise one explicitly with: python3 bump_firmware_versions.py
#
#   python3 tools/build_firmware.py --host-tests-only        runs stage 1 and stops
#   python3 tools/build_firmware.py --compile-only [TARGET]  runs stage 2 and stops (TARGET: all, g474 or h745)
#   python3 tools/build_firmware.py [--clean] [TARGET]       runs every stage that is ported and says which are not
#
# Environment: HYDRA_UMC_SKIP_HOST_TESTS=1 skips the logic tests of the host; HOST_CC names the host compiler (else cc, else gcc).
# =============================================================================
from __future__ import annotations

import argparse
import os
import platform
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Sequence

#: How a command is run: the arguments, whether its standard output is thrown away. Returns the exit status.
Runner = Callable[[Sequence[str], bool], int]

TWEETNACL = "@tweetnacl"


def default_runner(command: Sequence[str], quiet: bool) -> int:
    try:
        return subprocess.run(list(command), stdout=subprocess.DEVNULL if quiet else None, check=False).returncode
    except OSError:
        return 127


class Report:
    """The counters and the printed lines of the build (the same words as the shell script)."""

    def __init__(self, out: Callable[[str], None] = print) -> None:
        self.passed = 0
        self.warned = 0
        self.failed = 0
        self.out = out

    def ok(self, text: str) -> None:
        self.out(f"  OK   {text}")
        self.passed += 1

    def warn(self, text: str) -> None:
        self.out(f"  WARN {text}")
        self.warned += 1

    def fail(self, text: str) -> None:
        self.out(f"  FAIL {text}")
        self.failed += 1

    def step(self, title: str) -> None:
        self.out("")
        self.out(f"=== {title} ===")

    def summary(self) -> None:
        self.out("")
        self.out(f"{self.passed} passed, {self.warned} warnings, {self.failed} failed")


class BuildStopped(Exception):
    """A step failed: the summary is printed and the build ends with status 1, as the shell script does."""


@dataclass(frozen=True)
class Compile:
    """One compilation of a test of the host: `tests/<test>.c` plus `sources`, into `build/<binary>`."""

    binary: str
    test: str
    includes: tuple[str, ...] = ()
    defines: tuple[str, ...] = ()
    std: str = "c11"
    extra: tuple[str, ...] = ()
    sources: tuple[str, ...] = ()
    quiet: bool = True

    def command(self, root: str, cc: str, tweetnacl_object: str) -> list[str]:
        sources = [tweetnacl_object if s == TWEETNACL else f"{root}/{s}" for s in self.sources]
        return [
            cc,
            f"-std={self.std}",
            "-Wall",
            "-Wextra",
            *self.extra,
            *self.defines,
            *[f"-I{root}/{i}" for i in self.includes],
            "-o",
            f"{root}/build/{self.binary}",
            f"{root}/tests/{self.test}.c",
            *sources,
        ]


@dataclass(frozen=True)
class Case:
    """Compile one or more programs and run each; `ok` is printed when all of them passed.

    `compiled` is printed after the compilation when the shell script reports the compilation on its own line. `rerun_on_failure` runs a program
    again with its output shown when it failed. `syntax_only` is a command that is only compiled for its syntax (nothing is run).
    """

    ok: str
    compiles: tuple[Compile, ...] = ()
    compiled: str = ""
    rerun_on_failure: bool = False
    fail: str = ""
    fail_compile: str = ""
    syntax_only: tuple[str, ...] = ()


def _t(ok: str, binary: str, test: str, includes: Sequence[str] = (), defines: Sequence[str] = (), **kw) -> Case:
    quiet = kw.pop("quiet", True)
    case_kw = {k: kw.pop(k) for k in ("compiled", "rerun_on_failure", "fail", "fail_compile") if k in kw}
    return Case(ok=ok, compiles=(Compile(binary, test, tuple(includes), tuple(defines), quiet=quiet, **kw),), **case_kw)


H757_COMMON = "src/mcu_stm32h757/Common"
CM4 = "src/mcu_stm32h757/CM4"
CM4_BOOT = "src/mcu_stm32h757/CM4/boot"
G474 = "src/mcu_stm32g474"
G474_BOOT = "src/mcu_stm32g474/boot"
BOOT_DIRS = (G474_BOOT, CM4_BOOT, "src/mcu_stm32h757/CM7/boot")
GNU = {"std": "gnu11", "extra": ("-Wno-int-to-pointer-cast",)}


def host_cases(root: str) -> list[Case]:
    """The logic tests of the host, in the order of the shell script, with the words it prints."""
    cases: list[Case] = []
    cases.append(_t("relay_rx_queue host logic tests passed", "host_tests", "test_relay_rx_queue", ["tests", "src"], quiet=False))
    for d in (G474, CM4):
        cases.append(
            _t(
                f"dlc_to_fdcan_data_length host logic tests passed for {root}/{d}",
                "host_tests_dlc",
                "test_dlc_to_fdcan_data_length",
                [d],
                quiet=False,
                compiled=f"dlc_to_fdcan_data_length host test suite compiled for {root}/{d}",
            )
        )
    for d in BOOT_DIRS:
        cases.append(
            _t(
                f"boot_decision host logic tests passed for {root}/{d}",
                "host_tests_boot_decision",
                "test_boot_decision",
                [d],
                quiet=False,
                compiled=f"boot_decision host test suite compiled for {root}/{d}",
            )
        )
    cases.append(_t("flash_geometry host logic tests passed", "host_tests_flash_geometry", "test_flash_geometry", [G474_BOOT], quiet=False))
    cases.append(_t("relay_send_reassembly host logic tests passed", "host_tests_relay_send_reassembly", "test_relay_send_reassembly", [G474], quiet=False))
    cases.append(_t("relay_ids host logic tests passed", "host_tests_relay_ids", "test_relay_ids", [G474], quiet=False))
    cases.append(_t("spi_frame_crc host logic tests passed", "host_tests_spi_frame_crc", "test_spi_frame_crc", [CM4_BOOT], quiet=False))
    cases.append(_t("spi_boot_master host logic tests passed", "host_tests_spi_boot_master", "test_spi_boot_master", [H757_COMMON, "tests"], quiet=False))
    cases.append(_t("spi_boot_master_fsm host logic tests passed", "host_tests_spi_boot_master_fsm", "test_spi_boot_master_fsm", [H757_COMMON, "tests"], quiet=False))
    cases.append(_t("clock_tree host logic tests passed", "host_tests_clock_tree", "test_clock_tree", [H757_COMMON], ["-DHYDRA_PLATFORM_H757"], quiet=False))
    for variant, label in (
        ((), "(bank 1 only, the default)"),
        (("-DHYDRA_SDRAM_BANK2_ENABLED=1",), "-DHYDRA_SDRAM_BANK2_ENABLED=1"),
        (("-DHYDRA_SDRAM_BANK1_ENABLED=0", "-DHYDRA_SDRAM_BANK2_ENABLED=1"), "-DHYDRA_SDRAM_BANK1_ENABLED=0 -DHYDRA_SDRAM_BANK2_ENABLED=1"),
    ):
        cases.append(
            _t(
                f"sdram_init host logic tests passed {label}",
                "host_tests_sdram_init",
                "test_sdram_init",
                [H757_COMMON, "tests"],
                ["-DHYDRA_PLATFORM_H757", *variant],
                quiet=False,
            )
        )
    cases.append(_t("tmc_card host logic tests passed", "host_tests_tmc_card", "test_tmc_card", [H757_COMMON]))
    cases.append(_t("nor_flash host logic tests passed", "host_tests_nor_flash", "test_nor_flash", [H757_COMMON], ["-DHYDRA_PLATFORM_H757"], quiet=False))
    cases.append(_t("memtest host logic tests passed", "host_tests_memtest", "test_memtest", [H757_COMMON], quiet=False))
    cases.append(_t("clock_config host logic tests passed", "host_tests_clock_config", "test_clock_config", [H757_COMMON, "tests"], ["-DHYDRA_PLATFORM_H757"], quiet=False))
    for variant, label in (
        ((), "default"),
        (("-DHYDRA_PLATFORM_H757",), "-DHYDRA_PLATFORM_H757"),
        (("-DHYDRA_IPC_REQUIRE_CRC=1",), "-DHYDRA_IPC_REQUIRE_CRC=1"),
        (("-DHYDRA_PLATFORM_H757", "-DHYDRA_IPC_REQUIRE_CRC=1"), "-DHYDRA_PLATFORM_H757 -DHYDRA_IPC_REQUIRE_CRC=1"),
    ):
        cases.append(_t(f"ipc_link host logic tests passed [{label}]", "host_tests_ipc_link", "test_ipc_link", [H757_COMMON], list(variant), quiet=False))
    for variant in (("-DCORE_CM4",), ("-DCORE_CM7",), ("-DCORE_CM4", "-DHYDRA_PLATFORM_H757"), ("-DCORE_CM7", "-DHYDRA_PLATFORM_H757")):
        cases.append(
            _t(
                f"boot_roles host logic tests passed [{' '.join(variant)}]",
                "host_tests_boot_roles",
                "test_boot_roles",
                [H757_COMMON],
                list(variant),
                quiet=False,
            )
        )
    for n in (0, 1):
        cases.append(
            _t(
                f"test_ipc_mailbox host logic tests passed (HYDRA_IPC_REQUIRE_CRC={n})",
                f"host_tests_ipc_mailbox_{n}",
                "test_ipc_mailbox",
                [H757_COMMON, CM4_BOOT],
                [f"-DHYDRA_IPC_REQUIRE_CRC={n}"],
            )
        )
    app_flags = {
        0: ["-DHYDRA_FDCAN_KERNEL_CLOCK_EXPLICIT=0"],
        1: [
            "-DHYDRA_FDCAN_KERNEL_CLOCK_EXPLICIT=1",
            "-DHYDRA_APP_ENTER_BOOTLOADER=1",
            "-DHYDRA_HEALTH_FRAME=1",
            "-DHYDRA_BOOT_REPORT=1",
            "-DHYDRA_INIT_FAULT_LATCH=1",
        ],
    }
    for n in (0, 1):
        cases.append(_t(f"test_app_support host logic tests passed (switches on: {n})", f"host_tests_app_support_{n}", "test_app_support", [G474], app_flags[n]))
    cases.append(_t("spi_system host logic tests passed", "host_tests_spi_system", "test_spi_system", [CM4]))
    for n in (0, 1):
        cases.append(_t(f"spi_app host logic tests passed (switches on: {n})", f"host_tests_spi_app_{n}", "test_spi_app", [CM4, CM4_BOOT], [f"-DSPI_APP_TEST_SWITCHES={n}"]))
    cases.append(_t("rcc_owner host logic tests passed", "host_tests_rcc_owner", "test_rcc_owner", [CM4]))
    for d in BOOT_DIRS:
        cases.append(
            _t(
                f"flash_layer host logic tests passed ({d})",
                "host_tests_flash_layer_" + d.replace("/", "_"),
                "test_flash_layer",
                [d],
                ["-DHYDRA_TEST_ADOPTION"] if d == G474_BOOT else [],
            )
        )
    cases.append(
        Case(
            ok="estop_can host logic tests passed (controller board node, kinematic brain)",
            compiles=(
                Compile("host_tests_estop_can", "test_estop_can", ("tests", G474)),
                Compile("host_tests_estop_can", "test_estop_can", ("tests", CM4), ("-DESTOP_TEST_BRAIN",)),
            ),
        )
    )
    cases.append(_t("enabling_device host logic tests passed", "host_tests_enabling_device", "test_enabling_device", ["tests", CM4]))
    cases.append(
        Case(
            ok="zones_frame host logic tests passed (switch on and off)",
            compiles=(
                Compile("host_tests_zones_frame", "test_zones_frame", ("tests", CM4), ("-DHYDRA_ZONES_CAN_FILTER=1",)),
                Compile("host_tests_zones_frame", "test_zones_frame", ("tests", CM4)),
            ),
        )
    )
    cases.append(_t("expansion_slots host logic tests passed", "host_tests_expansion_slots", "test_expansion_slots", [H757_COMMON, "tests"]))
    cases.append(_t("host_link_master host logic tests passed", "host_tests_host_link_master", "test_host_link_master", [H757_COMMON, CM4_BOOT, "tests"]))
    cases.append(_t("task_watchdog host logic tests passed", "host_tests_task_watchdog", "test_task_watchdog", ["tests", CM4]))
    cases.append(_t("slot_scan_node host logic tests passed", "host_tests_slot_scan_node", "test_slot_scan_node", ["tests", G474_BOOT]))
    for n in (0, 1):
        cases.append(
            _t(
                f"fram_node host logic tests passed (switch {n})",
                f"host_tests_fram_node_{n}",
                "test_fram_node",
                [CM4],
                [f"-DHYDRA_FRAM_ERASE={n}", f"-DFRAM_TEST_SWITCH={n}"],
            )
        )
    cases.append(_t("fram_i2c host logic tests passed", "host_tests_fram_i2c", "test_fram_i2c", [CM4]))
    cases.append(_t("slot_scan_master host logic tests passed", "host_tests_slot_scan_master", "test_slot_scan_master", ["tests", G474_BOOT, CM4]))
    cases.append(_t("slot_scan_sweep host logic tests passed", "host_tests_slot_scan_sweep", "test_slot_scan_sweep", ["tests", CM4]))
    cases.append(_t("estop_input host logic tests passed", "host_tests_estop_input", "test_estop_input", [G474]))
    return cases


def bootloader_cases(root: str) -> list[Case]:
    """The tests of the bootloaders, which need the object of the vendored TweetNaCl (compiled before them, see `run_host_tests`)."""
    cases: list[Case] = []
    stubs = ("tests/stubs", G474_BOOT, "tests")
    cases.append(
        _t(
            "G474 bootloader SHA-256, Ed25519, CRC32, flash and metadata log host logic tests passed",
            "host_tests_bootloader_g474",
            "test_bootloader_g474_host",
            stubs,
            [],
            sources=(TWEETNACL, f"{G474_BOOT}/update_signature.c"),
            rerun_on_failure=True,
            **GNU,
        )
    )
    cases.append(
        _t(
            "G474 bootloader flash and metadata log host logic tests passed with the common flash layer (HYDRA_FLASH_LAYER=1)",
            "host_tests_bootloader_g474_flash_layer",
            "test_bootloader_g474_host",
            stubs,
            ["-DHYDRA_FLASH_LAYER=1"],
            sources=(TWEETNACL, f"{G474_BOOT}/update_signature.c"),
            rerun_on_failure=True,
            **GNU,
        )
    )
    for core in ("CM4", "CM7"):
        cases.append(
            _t(
                f"H745 {core} bootloader sector erase, CRC32 and SHA-256 host logic tests passed",
                f"host_tests_bootloader_h745_{core}",
                "test_bootloader_h745_host",
                ("tests/stubs", f"src/mcu_stm32h757/{core}/boot"),
                [f"-DHYDRA_TEST_{core}"],
                rerun_on_failure=True,
                **GNU,
            )
        )
    for core in ("CM4", "CM7"):
        cases.append(
            _t(
                f"H745 {core} bootloader sector erase through the common flash layer (HYDRA_FLASH_LAYER=1), CRC32 and SHA-256 host logic tests passed",
                f"host_tests_bootloader_h745_{core}_flash_layer",
                "test_bootloader_h745_host",
                ("tests/stubs", f"src/mcu_stm32h757/{core}/boot"),
                [f"-DHYDRA_TEST_{core}", "-DHYDRA_FLASH_LAYER=1"],
                rerun_on_failure=True,
                **GNU,
            )
        )
    for d in BOOT_DIRS:
        for test in ("test_image_header", "test_update_verify", "test_format2_decision"):
            cases.append(
                _t(
                    f"{test} host logic tests passed for {root}/{d}",
                    f"host_tests_{test}",
                    test,
                    (d, "tests"),
                    [],
                    extra=("-Werror",),
                    rerun_on_failure=True,
                )
            )
    for d in BOOT_DIRS:
        for flavour in ("HYDRA_TRUST_DEV", "HYDRA_TEST_RELEASE"):
            cases.append(
                _t(
                    f"trust table host tests passed ({flavour}) for {root}/{d}",
                    "host_tests_trust",
                    "test_bootloader_trust",
                    (d, "tests"),
                    [f"-D{flavour}"],
                    sources=(f"{d}/bootloader_trust.c", TWEETNACL),
                    rerun_on_failure=True,
                )
            )
    update_includes = ("tests/stubs/protocol", G474, G474_BOOT, "tests")
    cases.append(
        _t(
            "G474 bootloader whole-update host tests passed",
            "host_tests_bootloader_g474_update",
            "test_bootloader_g474_update_host",
            update_includes,
            [],
            sources=(TWEETNACL, f"{G474_BOOT}/update_signature.c"),
            **GNU,
        )
    )
    cases.append(
        _t(
            "G474 bootloader whole-update host tests passed with the common flash layer (HYDRA_FLASH_LAYER=1)",
            "host_tests_bootloader_g474_update_flash_layer",
            "test_bootloader_g474_update_host",
            update_includes,
            ["-DHYDRA_FLASH_LAYER=1"],
            sources=(TWEETNACL, f"{G474_BOOT}/update_signature.c"),
            **GNU,
        )
    )
    return cases


def protocol_commands(root: str, cc: str) -> list[tuple[str, list[str]]]:
    """The two bootloader protocol sources that are only compiled for their syntax against the stand-in HAL."""
    out = []
    for core in ("CM4", "CM7"):
        command = [
            cc,
            "-std=gnu11",
            "-fsyntax-only",
            "-Wall",
            "-Wno-int-to-pointer-cast",
            f"-I{root}/tests/stubs/h7protocol",
            f"-I{root}/src/mcu_stm32h757/{core}/boot",
            f"-I{root}/src/mcu_stm32h757/Common",
            f"-I{root}/src/mcu_stm32h757/{core}",
            f"-I{root}/src/mcu_stm32h757",
            f"-I{root}/tests",
            f"{root}/src/mcu_stm32h757/{core}/boot/bootloader_protocol.c",
        ]
        out.append((f"H745 {core} bootloader protocol source compiles against the stand-in HAL", command))
    return out


# =============================================================================
# Stage 2: the compilation and linking of the three boards (steps 2 to 10b of build_firmware.sh)
# =============================================================================

G4_HAL_REPO = "https://github.com/STMicroelectronics/stm32g4xx_hal_driver.git"
G4_HAL_TAG = "v1.2.7"
G4_CMSIS_DEVICE_REPO = "https://github.com/STMicroelectronics/cmsis_device_g4.git"
G4_CMSIS_DEVICE_TAG = "v1.2.6"
CMSIS_CORE_REPO = "https://github.com/STMicroelectronics/cmsis_core.git"
CMSIS_CORE_TAG = "v5.9.0_20250520"
H7_HAL_REPO = "https://github.com/STMicroelectronics/stm32h7xx_hal_driver.git"
H7_HAL_TAG = "v1.11.6"
H7_CMSIS_DEVICE_REPO = "https://github.com/STMicroelectronics/cmsis_device_h7.git"
H7_CMSIS_DEVICE_TAG = "v1.10.7"
FREERTOS_REPO = "https://github.com/FreeRTOS/FreeRTOS-Kernel.git"
FREERTOS_TAG = "V11.3.0"

FREERTOS_COMMON_SOURCES = ("tasks.c", "queue.c", "list.c", "timers.c", "event_groups.c")
HAL_MODULES_G4 = (
    "stm32g4xx_hal stm32g4xx_hal_cortex stm32g4xx_hal_gpio stm32g4xx_hal_rcc stm32g4xx_hal_rcc_ex stm32g4xx_hal_pwr stm32g4xx_hal_pwr_ex "
    "stm32g4xx_hal_flash stm32g4xx_hal_flash_ex stm32g4xx_hal_exti stm32g4xx_hal_fdcan stm32g4xx_hal_iwdg"
).split()
HAL_MODULES_H7 = (
    "stm32h7xx_hal stm32h7xx_hal_cortex stm32h7xx_hal_gpio stm32h7xx_hal_rcc stm32h7xx_hal_rcc_ex stm32h7xx_hal_pwr stm32h7xx_hal_pwr_ex "
    "stm32h7xx_hal_flash stm32h7xx_hal_flash_ex stm32h7xx_hal_exti stm32h7xx_hal_mdma stm32h7xx_hal_fdcan stm32h7xx_hal_iwdg stm32h7xx_hal_spi "
    "stm32h7xx_hal_hsem"
).split()
OPTIMIZE = ["-O2", "-Wall", "-ffunction-sections", "-fdata-sections"]
LINK_NOISE = ("not implemented", "note: the message", "in function `_")

#: How a command's standard output is read (the size of a binary): returns the text.
Capture = Callable[[Sequence[str]], str]
#: How a link command is run: the output of the linker that is only noise is dropped from what is shown, the exit status is its own.
LinkRunner = Callable[[Sequence[str]], int]


def default_capture(command: Sequence[str]) -> str:
    try:
        return subprocess.run(list(command), capture_output=True, text=True, check=False).stdout
    except OSError:
        return ""


def default_link(command: Sequence[str]) -> int:
    try:
        result = subprocess.run(list(command), stderr=subprocess.PIPE, text=True, check=False)
    except OSError:
        return 127
    for line in result.stderr.splitlines(keepends=True):
        if not any(noise in line for noise in LINK_NOISE):
            sys.stderr.write(line)
    return result.returncode


def sh_glob(directory: str, pattern: str) -> list[str]:
    """The files of a folder that match a pattern, in the order the shell lists them in the C locale (by byte)."""
    if not os.path.isdir(directory):
        return []
    import fnmatch

    return sorted(f"{directory}/{name}" for name in os.listdir(directory) if fnmatch.fnmatchcase(name, pattern))


def newer(source: str, target: str) -> bool:
    """True if `source` was modified after `target` (the test `-nt` of the shell)."""
    return os.path.exists(source) and (not os.path.exists(target) or os.path.getmtime(source) > os.path.getmtime(target))


def get_version_macro(header: str, macro: str) -> str:
    try:
        text = open(header, encoding="utf-8", errors="replace").read()
    except OSError:
        return ""
    import re

    return "\n".join(re.findall(r"define[ \t]+" + re.escape(macro) + r"[ \t]+([0-9]+)", text))


class Stage2:
    """Steps 2 to 10b. `build` is the Build that carries the report and the way commands are run."""

    def __init__(self, build: "Build", target: str = "all") -> None:
        self.b = build
        self.r = build.report
        self.run = build.run
        self.root = build.root
        self.target = target
        self.build_dir = f"{self.root}/build"
        self.out = f"{self.build_dir}/firmware_staging"
        self.final = f"{self.root}/firmware"
        self.freertos = f"{self.build_dir}/vendor/freertos"
        self.trust_defs: list[str] = []
        self.extra_cflags = (build.env.get("HYDRA_EXTRA_CFLAGS") or "").split()

    # -- helpers ---------------------------------------------------------------------------------------------------

    def stop(self) -> None:
        self.b.stop()

    @staticmethod
    def makedirs(*paths: str) -> None:
        for path in paths:
            os.makedirs(path, exist_ok=True)

    def cp(self, source: str, destination: str) -> bool:
        try:
            if os.path.isdir(destination):
                destination = f"{destination}/{os.path.basename(source)}"
            shutil.copyfile(source, destination)
            return True
        except OSError as error:
            print(f"cp: cannot copy '{source}': {error}", file=sys.stderr)
            return False

    def cp_glob(self, directory: str, pattern: str, destination: str, quiet: bool = False) -> None:
        for source in sh_glob(directory, pattern):
            if os.path.isfile(source):
                try:
                    shutil.copyfile(source, f"{destination}/{os.path.basename(source)}")
                except OSError as error:
                    if not quiet:
                        print(f"cp: cannot copy '{source}': {error}", file=sys.stderr)

    def gcc(self, flags: Sequence[str], *rest: str) -> int:
        return self.run(["arm-none-eabi-gcc", *flags, *rest], False)

    def component_version(self, header: str, prefix: str, label: str) -> None:
        parts = [get_version_macro(header, f"{prefix}_{part}") for part in ("MAJOR", "MINOR", "PATCH")]
        self.r.ok(f"{label} is at v{'.'.join(parts)} (source unchanged)")

    def version_tag(self, header: str, prefix: str) -> str:
        return "v" + ".".join(get_version_macro(header, f"{prefix}_{part}") for part in ("MAJOR", "MINOR", "PATCH"))

    def compile_dir(self, srcdir: str, outdir: str, cflags: Sequence[str], extra_inc: Sequence[str]) -> bool:
        for source in sh_glob(srcdir, "*.c"):
            name = os.path.basename(source)[:-2]
            if self.gcc(cflags, f"-I{srcdir}", *extra_inc, "-x", "c", "-c", source, "-o", f"{outdir}/{name}.o") != 0:
                return False
        return True

    def compile_freertos(self, cflags: Sequence[str], port_dir: str, objdir: str) -> bool:
        self.makedirs(objdir)
        include = f"-I{self.freertos}/include"
        for name in FREERTOS_COMMON_SOURCES:
            obj = f"{objdir}/{name}.o"
            if not os.path.isfile(obj) or newer(f"{self.freertos}/{name}", obj):
                if self.gcc(cflags, include, f"-I{port_dir}", "-x", "c", "-c", f"{self.freertos}/{name}", "-o", obj) != 0:
                    return False
        if self.gcc(cflags, include, f"-I{port_dir}", "-x", "c", "-c", f"{port_dir}/port.c", "-o", f"{objdir}/port.o") != 0:
            return False
        if self.gcc(cflags, include, f"-I{port_dir}", "-x", "c", "-c", f"{self.freertos}/portable/MemMang/heap_4.c", "-o", f"{objdir}/heap_4.o") != 0:
            return False
        return True

    def build_bin_hex(self, elf: str) -> None:
        stem = elf[: -len(".elf")] if elf.endswith(".elf") else elf
        self.run(["arm-none-eabi-objcopy", "-O", "binary", elf, f"{stem}.bin"], False)
        self.run(["arm-none-eabi-objcopy", "-O", "ihex", elf, f"{stem}.hex"], False)

    def text_size(self, elf: str) -> str:
        lines = [line for line in self.b.capture(["arm-none-eabi-size", elf]).splitlines() if line.strip()]
        return lines[-1].split()[0] if lines and lines[-1].split() else ""

    def stage(self, source_dir: str, name: str) -> None:
        for extension in ("elf", "bin", "hex"):
            self.cp(f"{source_dir}/{name}.{extension}", self.out)

    def link(self, ldflags: Sequence[str], script: str, objects: Sequence[str], elf: str) -> bool:
        return self.b.link([ "arm-none-eabi-gcc", *ldflags, f"-T{script}", *objects, "-o", elf]) == 0

    def finish_failure(self, text: str) -> None:
        self.r.fail(text)
        self.stop()

    def fetch(self, label: str, url: str, tag: str, destination: str, cached: str, announce: str, passed: str, sparse: bool = False) -> None:
        if not os.path.isdir(destination):
            print(announce)
            if sparse:
                self.run(["git", "-c", "safe.directory=*", "clone", "--depth", "1", "--filter=blob:none", "--no-checkout", "-q", "--branch", tag, url, destination], False)
                self.run(["git", "-C", destination, "-c", "safe.directory=*", "sparse-checkout", "init", "--no-cone"], False)
                os.makedirs(f"{destination}/.git/info", exist_ok=True)
                with open(f"{destination}/.git/info/sparse-checkout", "w", encoding="utf-8", newline="\n") as handle:
                    handle.write("/Core/Include/**\n")
                self.run(["git", "-C", destination, "-c", "safe.directory=*", "checkout", "-q"], False)
            else:
                self.run(["git", "-c", "safe.directory=*", "clone", "--depth", "1", "--branch", tag, "-q", url, destination], False)
            self.r.ok(passed)
        else:
            self.r.ok(cached)

    # -- the steps -------------------------------------------------------------------------------------------------

    def run_all(self) -> None:
        self.makedirs(self.out, self.final)
        for pattern in ("HYDRA_*.bin", "HYDRA_*.elf", "HYDRA_*.hex"):
            for path in sh_glob(self.out, pattern):
                os.remove(path)
        self.step2()
        if self.target in ("all", "g474"):
            self.step3()
            self.step4()
            self.step5()
            self.step6()
        if self.target in ("all", "h745"):
            self.step7()
            self.step8()
            self.step9()
            self.step10()
            self.step10b()

    def step2(self) -> None:
        self.r.step("2. FreeRTOS kernel sources (shared by every application target - see docs/architecture.md)")
        self.fetch(
            "FreeRTOS",
            FREERTOS_REPO,
            FREERTOS_TAG,
            self.freertos,
            "FreeRTOS-Kernel already cached at build/vendor/freertos",
            f"Fetching FreeRTOS-Kernel ({FREERTOS_TAG})...",
            "FreeRTOS-Kernel fetched",
        )

    def assemble_includes(self, base: str, hal_prefix: str, device: str, core_header: str, label: str) -> None:
        """The copies that put the HAL, the CMSIS device headers and the CMSIS core headers in one tree (the lines after the fetches)."""
        vendor = f"{base}/vendor"
        hal_inc = f"{base}/common/HAL_Include"
        self.cp_glob(f"{vendor}/hal/Inc", "*.h", hal_inc, quiet=True)
        legacy = f"{vendor}/hal/Inc/Legacy"
        if os.path.isdir(legacy):
            shutil.copytree(legacy, f"{hal_inc}/Legacy", dirs_exist_ok=True)
        self.cp(f"{hal_inc}/{hal_prefix}_hal_conf_template.h", f"{hal_inc}/{hal_prefix}_hal_conf.h")
        self.cp_glob(f"{vendor}/hal/Src", "*.c", f"{base}/hal_src")
        self.cp_glob(f"{vendor}/{device}/Include", "*.h", f"{base}/common/CMSIS_Include")
        core_include = f"{vendor}/cmsis_core/Core/Include"
        if os.path.isdir(core_include):
            for entry in sorted(os.listdir(core_include)):
                path = f"{core_include}/{entry}"
                if os.path.isdir(path):
                    shutil.copytree(path, f"{base}/common/CMSIS_Include/{entry}", dirs_exist_ok=True)
                else:
                    shutil.copyfile(path, f"{base}/common/CMSIS_Include/{entry}")
        if os.path.isfile(f"{base}/common/CMSIS_Include/{core_header}") and os.path.isfile(f"{hal_inc}/{hal_prefix}_hal.h"):
            self.r.ok("HAL/CMSIS include tree assembled")
        else:
            self.r.fail(f"HAL/CMSIS include tree incomplete - check {label} manually")

    def step3(self) -> None:
        self.r.step("3. Robot Controller Board (STM32G474RET6) - ST HAL/CMSIS sources")
        g4 = f"{self.build_dir}/g474"
        self.makedirs(
            f"{g4}/vendor", f"{g4}/common/HAL_Include", f"{g4}/common/CMSIS_Include", f"{g4}/hal_src", f"{g4}/hal_obj", f"{g4}/app", f"{g4}/boot_obj",
            f"{g4}/app_obj", f"{g4}/freertos_obj",
        )
        self.fetch("hal", G4_HAL_REPO, G4_HAL_TAG, f"{g4}/vendor/hal", "HAL driver already cached at build/g474/vendor/hal", f"Fetching STM32G4xx HAL driver ({G4_HAL_TAG})...", "HAL driver fetched")
        self.fetch(
            "cmsis_device",
            G4_CMSIS_DEVICE_REPO,
            G4_CMSIS_DEVICE_TAG,
            f"{g4}/vendor/cmsis_device_g4",
            "CMSIS device (G4) already cached at build/g474/vendor/cmsis_device_g4",
            f"Fetching CMSIS device headers for G4 ({G4_CMSIS_DEVICE_TAG})...",
            "CMSIS device (G4) fetched",
        )
        self.fetch(
            "cmsis_core",
            CMSIS_CORE_REPO,
            CMSIS_CORE_TAG,
            f"{g4}/vendor/cmsis_core",
            "CMSIS core already cached at build/g474/vendor/cmsis_core",
            "Fetching generic ARM CMSIS Core headers (Include/ only, sparse)...",
            "CMSIS core fetched (Include/ only)",
            sparse=True,
        )
        self.assemble_includes(g4, "stm32g4xx", "cmsis_device_g4", "core_cm4.h", "build/g474/common/")

    def step4(self) -> None:
        r = self.r
        r.step("4. Robot Controller Board - common compiler flags and shared HAL objects")
        if self.b.env.get("HYDRA_UMC_RELEASE", "0") == "1":
            self.trust_defs = []
            r.ok("bootloaders are built with the release trust table (closed until the image-signing key is in docs/image_trust.json)")
        else:
            self.trust_defs = ["-DHYDRA_TRUST_DEV"]
            r.warn("bootloaders trust the DEVELOPMENT image key, whose seed is public: do not put these images on a board that anybody else can reach")
        g4 = f"{self.build_dir}/g474"
        self.g4 = g4
        self.cflags_g4 = [
            "-mcpu=cortex-m4", "-mthumb", "-mfpu=fpv4-sp-d16", "-mfloat-abi=hard", "-DSTM32G474xx", "-DUSE_HAL_DRIVER",
            f"-I{g4}/common/CMSIS_Include", f"-I{g4}/common/HAL_Include", *OPTIMIZE, *self.trust_defs, *self.extra_cflags,
        ]
        self.ld_g4 = ["-mcpu=cortex-m4", "-mthumb", "-mfpu=fpv4-sp-d16", "-mfloat-abi=hard", "-specs=nano.specs", "-specs=nosys.specs", "-Wl,--gc-sections"]
        self.cflags_g4_app = [*self.cflags_g4, f"-I{self.root}/src/mcu_stm32g474", f"-I{self.freertos}/include", f"-I{self.freertos}/portable/GCC/ARM_CM4F"]
        hal_ok = True
        for name in HAL_MODULES_G4:
            obj = f"{g4}/hal_obj/{name}.o"
            if not os.path.isfile(obj) or newer(f"{g4}/hal_src/{name}.c", obj):
                if self.gcc(self.cflags_g4, "-x", "c", "-c", f"{g4}/hal_src/{name}.c", "-o", obj) != 0:
                    hal_ok = False
        count = len(sh_glob(f"{g4}/hal_obj", "*.o"))
        if hal_ok and count >= 1:
            r.ok(f"{count} HAL modules compiled")
        else:
            self.finish_failure("one or more HAL modules failed to compile - see errors above")
        self.cp(f"{g4}/vendor/cmsis_device_g4/Source/Templates/gcc/startup_stm32g474xx.s", f"{g4}/app")
        self.cp(f"{g4}/vendor/cmsis_device_g4/Source/Templates/system_stm32g4xx.c", f"{g4}/app")
        self.gcc(self.cflags_g4, "-x", "assembler-with-cpp", "-c", f"{g4}/app/startup_stm32g474xx.s", "-o", f"{g4}/app/startup.o")
        self.gcc(self.cflags_g4, "-x", "c", "-c", f"{g4}/app/system_stm32g4xx.c", "-o", f"{g4}/app/system_stm32g4xx.o")
        r.ok("startup + system files compiled")
        if self.compile_freertos(self.cflags_g4_app, f"{self.freertos}/portable/GCC/ARM_CM4F", f"{g4}/freertos_obj"):
            r.ok("FreeRTOS kernel (ARM_CM4F port) compiled for Robot Controller Board")
        else:
            self.finish_failure("FreeRTOS kernel failed to compile for Robot Controller Board - see errors above")

    def step5(self) -> None:
        r = self.r
        r.step("5. Robot Controller Board bootloader (src/mcu_stm32g474/boot/) - bare-metal CAN-OTA, no FreeRTOS")
        g4 = self.g4
        src = f"{self.root}/src/mcu_stm32g474/boot"
        header = f"{src}/bootloader_common.h"
        self.component_version(header, "BOOTLOADER_VERSION", "Robot Controller Board bootloader")
        for path in sh_glob(f"{g4}/boot_obj", "*.o"):
            os.remove(path)
        if self.compile_dir(src, f"{g4}/boot_obj", self.cflags_g4, [f"-I{self.root}/src/mcu_stm32g474"]):
            r.ok(f"bootloader_*.c compiled ({len(sh_glob(src, '*.c'))} files)")
        else:
            self.finish_failure("Robot Controller Board bootloader failed to compile - see errors above")
        name = f"HYDRA_RCB_BOOTLOADER_{self.version_tag(header, 'BOOTLOADER_VERSION')}"
        elf = f"{g4}/boot_obj/{name}.elf"
        objects = [f"{g4}/app/startup.o", f"{g4}/app/system_stm32g4xx.o", *sh_glob(f"{g4}/boot_obj", "*.o"), *sh_glob(f"{g4}/hal_obj", "*.o")]
        if not self.link(self.ld_g4, f"{src}/STM32G474RETx_BOOTLOADER.ld", objects, elf):
            self.finish_failure("Robot Controller Board bootloader: link failed - see errors above")
        self.build_bin_hex(elf)
        self.stage(f"{g4}/boot_obj", name)
        r.ok(f"{name}.bin/.hex/.elf built ({self.text_size(elf)} bytes text)")

    def step6(self) -> None:
        r = self.r
        r.step("6. Robot Controller Board application (src/mcu_stm32g474/) - FreeRTOS")
        g4 = self.g4
        src = f"{self.root}/src/mcu_stm32g474"
        header = f"{src}/boot/bootloader_common.h"
        self.component_version(header, "FIRMWARE_VERSION", "Robot Controller Board application")
        for path in sh_glob(f"{g4}/app_obj", "*.o"):
            os.remove(path)
        self.gcc(self.cflags_g4_app, f"-I{src}", "-x", "c", "-c", f"{src}/STM32G474RE_main.c", "-o", f"{g4}/app_obj/STM32G474RE_main.o")
        self.gcc(self.cflags_g4_app, f"-I{src}", f"-I{src}/boot", "-x", "c", "-c", f"{src}/RobotControllerRelay.c", "-o", f"{g4}/app_obj/RobotControllerRelay.o")
        self.makedirs(f"{g4}/app_check")
        switches = ["-DHYDRA_HEALTH_FRAME=1", "-DHYDRA_INIT_FAULT_LATCH=1", "-DHYDRA_BOOT_REPORT=1", "-DHYDRA_FDCAN_KERNEL_CLOCK_EXPLICIT=1"]
        if self.gcc(
            self.cflags_g4_app, *switches, f"-I{src}", f"-I{src}/boot", "-x", "c", "-c", f"{src}/RobotControllerRelay.c",
            "-o", f"{g4}/app_check/RobotControllerRelay_all_switches.o",
        ) == 0:
            r.ok("RobotControllerRelay.c compiles with the health frame, the init fault latch, the boot report and the explicit FDCAN clock on")
        else:
            self.finish_failure("RobotControllerRelay.c does not compile with those switches on")
        name = f"HYDRA_RCB_APP_{self.version_tag(header, 'FIRMWARE_VERSION')}"
        elf = f"{g4}/app_obj/{name}.elf"
        objects = [
            f"{g4}/app/startup.o", f"{g4}/app/system_stm32g4xx.o", *sh_glob(f"{g4}/app_obj", "*.o"), *sh_glob(f"{g4}/hal_obj", "*.o"),
            *sh_glob(f"{g4}/freertos_obj", "*.o"),
        ]
        if not self.link(self.ld_g4, f"{src}/STM32G474RETx_APP.ld", objects, elf):
            self.finish_failure("Robot Controller Board application: link failed - see errors above")
        self.build_bin_hex(elf)
        self.stage(f"{g4}/app_obj", name)
        r.ok(
            f"{name}.bin/.hex/.elf built ({self.text_size(elf)} bytes text) - real AXIS_STATUS responder + Tier 2/3 relay tunnel "
            "(RobotControllerRelay.c), still HSI clock/no real motion tasks"
        )

    def step7(self) -> None:
        self.r.step("7. Kinematic Brain (STM32H745ZIT6) - ST HAL/CMSIS sources")
        h7 = f"{self.build_dir}/h745"
        self.h7 = h7
        self.makedirs(
            f"{h7}/vendor", f"{h7}/common/HAL_Include", f"{h7}/common/CMSIS_Include", f"{h7}/hal_src", f"{h7}/hal_obj", f"{h7}/cm7", f"{h7}/cm4",
            f"{h7}/cm7_boot_obj", f"{h7}/cm7_app_obj", f"{h7}/cm4_boot_obj", f"{h7}/cm4_app_obj", f"{h7}/cm7_freertos_obj", f"{h7}/cm4_freertos_obj",
        )
        self.fetch("hal", H7_HAL_REPO, H7_HAL_TAG, f"{h7}/vendor/hal", "HAL driver already cached at build/h745/vendor/hal", f"Fetching STM32H7xx HAL driver ({H7_HAL_TAG})...", "HAL driver fetched")
        self.fetch(
            "cmsis_device",
            H7_CMSIS_DEVICE_REPO,
            H7_CMSIS_DEVICE_TAG,
            f"{h7}/vendor/cmsis_device_h7",
            "CMSIS device (H7) already cached at build/h745/vendor/cmsis_device_h7",
            f"Fetching CMSIS device headers for H7 ({H7_CMSIS_DEVICE_TAG})...",
            "CMSIS device (H7) fetched",
        )
        self.fetch(
            "cmsis_core",
            CMSIS_CORE_REPO,
            CMSIS_CORE_TAG,
            f"{h7}/vendor/cmsis_core",
            "CMSIS core already cached at build/h745/vendor/cmsis_core",
            "Fetching generic ARM CMSIS Core headers (Include/ only, sparse)...",
            "CMSIS core fetched (Include/ only)",
            sparse=True,
        )
        self.assemble_includes(h7, "stm32h7xx", "cmsis_device_h7", "core_cm7.h", "build/h745/common/")

    def h7_flags(self, core: str) -> list[str]:
        h7 = self.h7
        if core == "CM7":
            cpu = ["-mcpu=cortex-m7", "-mthumb", "-mfpu=fpv5-d16", "-mfloat-abi=hard"]
        else:
            cpu = ["-mcpu=cortex-m4", "-mthumb", "-mfpu=fpv4-sp-d16", "-mfloat-abi=hard"]
        return [*cpu, "-DSTM32H757xx", f"-DCORE_{core}", f"-I{h7}/common/CMSIS_Include", f"-I{h7}/common/HAL_Include", *OPTIMIZE, *self.trust_defs, *self.extra_cflags]

    def step8(self) -> None:
        r = self.r
        r.step("8. Kinematic Brain - common compiler flags and shared HAL objects (per core)")
        h7 = self.h7
        if not self.trust_defs and self.b.env.get("HYDRA_UMC_RELEASE", "0") != "1":
            self.trust_defs = ["-DHYDRA_TRUST_DEV"]
        for core in ("CM7", "CM4"):
            flags = self.h7_flags(core)
            lower = core.lower()
            objdir = f"{h7}/hal_obj_{lower}"
            self.makedirs(objdir)
            hal_ok = True
            for name in HAL_MODULES_H7:
                obj = f"{objdir}/{name}.o"
                if not os.path.isfile(obj) or newer(f"{h7}/hal_src/{name}.c", obj):
                    if self.gcc(flags, "-x", "c", "-c", f"{h7}/hal_src/{name}.c", "-o", obj) != 0:
                        hal_ok = False
            count = len(sh_glob(objdir, "*.o"))
            if hal_ok and count >= 1:
                r.ok(f"{core}: {count} HAL modules compiled")
            else:
                self.finish_failure(f"{core}: one or more HAL modules failed to compile - see errors above")
            core_dir = f"{h7}/{lower}"
            self.cp(f"{h7}/vendor/cmsis_device_h7/Source/Templates/gcc/startup_stm32h757xx.s", core_dir)
            self.cp(f"{h7}/vendor/cmsis_device_h7/Source/Templates/system_stm32h7xx_dualcore_boot_cm4_cm7.c", f"{core_dir}/system_stm32h7xx.c")
            self.gcc(flags, "-x", "assembler-with-cpp", "-c", f"{core_dir}/startup_stm32h757xx.s", "-o", f"{core_dir}/startup.o")
            self.gcc(flags, "-x", "c", "-c", f"{core_dir}/system_stm32h7xx.c", "-o", f"{core_dir}/system_stm32h7xx.o")
            r.ok(f"{core}: startup + system files compiled")
        self.cflags_cm7 = self.h7_flags("CM7")
        self.cflags_cm4 = self.h7_flags("CM4")
        self.ld_cm7 = ["-mcpu=cortex-m7", "-mthumb", "-mfpu=fpv5-d16", "-mfloat-abi=hard", "-specs=nano.specs", "-specs=nosys.specs", "-Wl,--gc-sections"]
        self.ld_cm4 = ["-mcpu=cortex-m4", "-mthumb", "-mfpu=fpv4-sp-d16", "-mfloat-abi=hard", "-specs=nano.specs", "-specs=nosys.specs", "-Wl,--gc-sections"]
        self.cflags_cm7_app = [*self.cflags_cm7, f"-I{self.root}/src/mcu_stm32h757/CM7", f"-I{self.freertos}/include", f"-I{self.freertos}/portable/GCC/ARM_CM7/r0p1"]
        self.cflags_cm4_app = [*self.cflags_cm4, f"-I{self.root}/src/mcu_stm32h757/CM4", f"-I{self.freertos}/include", f"-I{self.freertos}/portable/GCC/ARM_CM4F"]
        if self.compile_freertos(self.cflags_cm7_app, f"{self.freertos}/portable/GCC/ARM_CM7/r0p1", f"{h7}/cm7_freertos_obj"):
            r.ok("FreeRTOS kernel (ARM_CM7/r0p1 port) compiled for CM7")
        else:
            self.finish_failure("FreeRTOS kernel failed to compile for CM7 - see errors above")
        if self.compile_freertos(self.cflags_cm4_app, f"{self.freertos}/portable/GCC/ARM_CM4F", f"{h7}/cm4_freertos_obj"):
            r.ok("FreeRTOS kernel (ARM_CM4F port) compiled for CM4")
        else:
            self.finish_failure("FreeRTOS kernel failed to compile for CM4 - see errors above")

    def step9(self) -> None:
        r = self.r
        r.step("9. Kinematic Brain CM7 bootloader (bare-metal CAN-OTA, mailbox-relayed) + application (FreeRTOS) - src/mcu_stm32h757/CM7/")
        h7 = self.h7
        src = f"{self.root}/src/mcu_stm32h757/CM7"
        self.common_inc = [f"-I{self.root}/src/mcu_stm32h757/Common"]
        header = f"{src}/boot/bootloader_common.h"
        self.component_version(header, "BOOTLOADER_VERSION", "Kinematic Brain CM7 bootloader")
        if self.compile_dir(f"{src}/boot", f"{h7}/cm7_boot_obj", self.cflags_cm7, self.common_inc):
            r.ok(f"CM7 bootloader_*.c compiled ({len(sh_glob(f'{src}/boot', '*.c'))} files)")
        else:
            self.finish_failure("CM7 bootloader failed to compile - see errors above")
        name = f"HYDRA_KB_CM7_BOOTLOADER_{self.version_tag(header, 'BOOTLOADER_VERSION')}"
        elf = f"{h7}/cm7_boot_obj/{name}.elf"
        objects = [f"{h7}/cm7/startup.o", f"{h7}/cm7/system_stm32h7xx.o", *sh_glob(f"{h7}/cm7_boot_obj", "*.o"), *sh_glob(f"{h7}/hal_obj_cm7", "*.o")]
        if not self.link(self.ld_cm7, f"{src}/boot/STM32H757XIHx_CM7_BOOTLOADER.ld", objects, elf):
            self.finish_failure("CM7 bootloader: link failed - see errors above")
        self.build_bin_hex(elf)
        self.stage(f"{h7}/cm7_boot_obj", name)
        r.ok(f"{name}.bin/.hex/.elf built ({self.text_size(elf)} bytes text)")
        self.component_version(header, "FIRMWARE_VERSION", "Kinematic Brain CM7 application")
        self.gcc(self.cflags_cm7_app, f"-I{src}", "-x", "c", "-c", f"{src}/STM32H757XI_CM7_main.c", "-o", f"{h7}/cm7_app_obj/STM32H757XI_CM7_main.o")
        name = f"HYDRA_KB_CM7_APP_{self.version_tag(header, 'FIRMWARE_VERSION')}"
        elf = f"{h7}/cm7_app_obj/{name}.elf"
        objects = [
            f"{h7}/cm7/startup.o", f"{h7}/cm7/system_stm32h7xx.o", *sh_glob(f"{h7}/cm7_app_obj", "*.o"), *sh_glob(f"{h7}/hal_obj_cm7", "*.o"),
            *sh_glob(f"{h7}/cm7_freertos_obj", "*.o"),
        ]
        if not self.link(self.ld_cm7, f"{src}/STM32H757XIHx_CM7_APP.ld", objects, elf):
            self.finish_failure("CM7 application: link failed - see errors above")
        self.build_bin_hex(elf)
        self.stage(f"{h7}/cm7_app_obj", name)
        r.ok(f"{name}.bin/.hex/.elf built ({self.text_size(elf)} bytes text) - FreeRTOS GPIO-toggle smoke test")

    def step10(self) -> None:
        r = self.r
        r.step("10. Kinematic Brain CM4 bootloader (bare-metal CAN-OTA gateway: SPI1+FDCAN1+mailbox) + application (FreeRTOS) - src/mcu_stm32h757/CM4/")
        h7 = self.h7
        src = f"{self.root}/src/mcu_stm32h757/CM4"
        header = f"{src}/boot/bootloader_common.h"
        self.component_version(header, "BOOTLOADER_VERSION", "Kinematic Brain CM4 bootloader")
        if self.compile_dir(f"{src}/boot", f"{h7}/cm4_boot_obj", self.cflags_cm4, [*self.common_inc, f"-I{src}"]):
            r.ok(f"CM4 bootloader_*.c compiled ({len(sh_glob(f'{src}/boot', '*.c'))} files)")
        else:
            self.finish_failure("CM4 bootloader failed to compile - see errors above")
        name = f"HYDRA_KB_CM4_BOOTLOADER_{self.version_tag(header, 'BOOTLOADER_VERSION')}"
        elf = f"{h7}/cm4_boot_obj/{name}.elf"
        objects = [f"{h7}/cm4/startup.o", f"{h7}/cm4/system_stm32h7xx.o", *sh_glob(f"{h7}/cm4_boot_obj", "*.o"), *sh_glob(f"{h7}/hal_obj_cm4", "*.o")]
        if not self.link(self.ld_cm4, f"{src}/boot/STM32H757XIHx_CM4_BOOTLOADER.ld", objects, elf):
            self.finish_failure("CM4 bootloader: link failed - see errors above")
        self.build_bin_hex(elf)
        self.stage(f"{h7}/cm4_boot_obj", name)
        r.ok(f"{name}.bin/.hex/.elf built ({self.text_size(elf)} bytes text)")
        self.component_version(header, "FIRMWARE_VERSION", "Kinematic Brain CM4 application")
        app = self.cflags_cm4_app
        self.gcc(app, f"-I{src}", f"-I{src}/boot", "-x", "c", "-c", f"{src}/STM32H757XI_CM4_main.c", "-o", f"{h7}/cm4_app_obj/STM32H757XI_CM4_main.o")
        self.gcc(app, f"-I{src}", f"-I{src}/boot", *self.common_inc, "-x", "c", "-c", f"{src}/KinematicBrainCan.c", "-o", f"{h7}/cm4_app_obj/KinematicBrainCan.o")
        self.gcc(app, f"-I{src}", f"-I{src}/boot", "-x", "c", "-c", f"{src}/SpiApp.c", "-o", f"{h7}/cm4_app_obj/SpiApp.o")
        self.makedirs(f"{h7}/cm4_app_check")
        all_switches = [
            "-DHYDRA_SPI_SYSTEM=1", "-DHYDRA_SPI_STOP_COMMAND=1", "-DHYDRA_SPI_APP=1", "-DHYDRA_SPI_REQUIRE_CRC=1", "-DHYDRA_SPI_ZONES_HEARTBEAT=1",
            "-DHYDRA_SPI_SLOT_SCAN=1",
        ]
        if self.gcc(app, *all_switches, f"-I{src}", f"-I{src}/boot", "-x", "c", "-c", f"{src}/SpiApp.c", "-o", f"{h7}/cm4_app_check/SpiApp_all_switches.o") == 0:
            r.ok("SpiApp.c compiles with every switch on (SPI service, stop command, seal, zones heartbeat, full slot scan)")
        else:
            self.finish_failure("SpiApp.c does not compile with every switch on")
        fram = ["-DHYDRA_SPI_SYSTEM=1", "-DHYDRA_SPI_APP=1", "-DHYDRA_FRAM_ERASE=1", "-DHYDRA_FRAM_I2C_TIMING=0x10000001"]
        if self.gcc(app, *fram, f"-I{src}", f"-I{src}/boot", "-x", "c", "-c", f"{src}/SpiApp.c", "-o", f"{h7}/cm4_app_check/SpiApp_fram.o") == 0:
            r.ok("SpiApp.c compiles with the F-RAM driver on (I2C4, FM24CL64B)")
        else:
            self.finish_failure("SpiApp.c does not compile with the F-RAM driver on")
        name = f"HYDRA_KB_CM4_APP_{self.version_tag(header, 'FIRMWARE_VERSION')}"
        elf = f"{h7}/cm4_app_obj/{name}.elf"
        objects = [
            f"{h7}/cm4/startup.o", f"{h7}/cm4/system_stm32h7xx.o", *sh_glob(f"{h7}/cm4_app_obj", "*.o"), *sh_glob(f"{h7}/hal_obj_cm4", "*.o"),
            *sh_glob(f"{h7}/cm4_freertos_obj", "*.o"),
        ]
        if not self.link(self.ld_cm4, f"{src}/STM32H757XIHx_CM4_APP.ld", objects, elf):
            self.finish_failure("CM4 application: link failed - see errors above")
        self.build_bin_hex(elf)
        self.stage(f"{h7}/cm4_app_obj", name)
        r.ok(
            f"{name}.bin/.hex/.elf built ({self.text_size(elf)} bytes text) - real PLL1 clock config + real FDCAN1 STACK A + Tier 2/3 relay tunnel, "
            "still no CM7<->CM4 scheduler sync"
        )

    def step10b(self) -> None:
        r = self.r
        r.step("10b. Kinematic Brain bootloaders and CAN code compiled for the H757 platform (compile check only, nothing is linked or shipped)")
        h7 = self.h7
        root = self.root
        failed = False
        for core in ("CM7", "CM4"):
            obj = f"{h7}/{core.lower()}_boot_h757_check_obj"
            self.makedirs(obj)
            if core == "CM7":
                flags, include = self.cflags_cm7, list(self.common_inc)
            else:
                flags, include = self.cflags_cm4, [*self.common_inc, f"-I{root}/src/mcu_stm32h757/CM4"]
            if self.compile_dir(f"{root}/src/mcu_stm32h757/{core}/boot", obj, [*flags, "-DHYDRA_PLATFORM_H757"], include):
                r.ok(f"{core} bootloader sources compile for the H757 platform")
            else:
                r.fail(f"{core} bootloader sources do NOT compile for the H757 platform - see errors above")
                failed = True
        app_obj = f"{h7}/cm4_app_h757_check_obj"
        self.makedirs(app_obj)
        if self.gcc(
            self.cflags_cm4_app, "-DHYDRA_PLATFORM_H757", f"-I{root}/src/mcu_stm32h757/CM4", f"-I{root}/src/mcu_stm32h757/CM4/boot", *self.common_inc,
            "-x", "c", "-c", f"{root}/src/mcu_stm32h757/CM4/KinematicBrainCan.c", "-o", f"{app_obj}/KinematicBrainCan.o",
        ) == 0:
            r.ok("CM4 CAN application code compiles for the H757 platform")
        else:
            r.fail("CM4 CAN application code does NOT compile for the H757 platform - see errors above")
            failed = True
        link_obj = f"{h7}/cm7_host_link_h757_check_obj"
        self.makedirs(link_obj)
        if self.gcc(
            self.cflags_cm7, "-DHYDRA_PLATFORM_H757", "-DHYDRA_HOST_LINK_MASTER=1", f"-I{root}/src/mcu_stm32h757/CM7", f"-I{root}/src/mcu_stm32h757/CM4/boot",
            *self.common_inc, "-x", "c", "-c", f"{root}/src/mcu_stm32h757/CM7/HostLinkSpi3.c", "-o", f"{link_obj}/HostLinkSpi3.o",
        ) == 0:
            r.ok("CM7 driver of the host link (SPI3 master) compiles for the H757 platform")
        else:
            r.fail("CM7 driver of the host link (SPI3 master) does NOT compile for the H757 platform - see errors above")
            failed = True
        if failed:
            self.stop()


class Build:
    def __init__(
        self,
        root: Path | str,
        report: Report,
        runner: Runner = default_runner,
        which: Callable[[str], str | None] = shutil.which,
        env: dict | None = None,
        capture: "Capture | None" = None,
        link: "LinkRunner | None" = None,
    ) -> None:
        self.root = str(root).replace("\\", "/")
        self.report = report
        self.run = runner
        self.capture = capture or default_capture
        self.link = link or default_link
        self.which = which
        self.env = os.environ if env is None else env

    # -- helpers ---------------------------------------------------------------------------------------------------

    def stop(self) -> None:
        self.report.summary()
        raise BuildStopped

    def host_compiler(self) -> str | None:
        wanted = self.env.get("HOST_CC") or "cc"
        if self.which(wanted) is None and self.which("gcc") is None and not os.path.isfile(wanted):
            return None
        return self.env.get("HOST_CC") or self.which("cc") or self.which("gcc")

    # -- stage 1 ---------------------------------------------------------------------------------------------------

    def toolchain(self) -> None:
        r = self.report
        r.step("1. Toolchain")
        if self.which("arm-none-eabi-gcc") is None:
            print("arm-none-eabi-gcc not found - attempting install via apt...")
            if self.which("apt"):
                self.run(["sudo", "apt", "update"], False)
                self.run(
                    [
                        "sudo", "apt", "install", "-y", "gcc-arm-none-eabi", "binutils-arm-none-eabi", "libnewlib-arm-none-eabi",
                        "libstdc++-arm-none-eabi-newlib", "libstdc++-arm-none-eabi-dev", "git",
                    ],
                    False,
                )
            else:
                r.fail("no apt available and arm-none-eabi-gcc is missing - install the ARM GNU Toolchain manually, then re-run this script")
                self.stop()
        if self.which("arm-none-eabi-gcc"):
            r.ok(f"arm-none-eabi-gcc found: {self._first_line(['arm-none-eabi-gcc', '--version'])}")
        else:
            r.fail("arm-none-eabi-gcc still not found after install attempt")
            self.stop()
        for tool in ("arm-none-eabi-objcopy", "arm-none-eabi-size", "arm-none-eabi-nm"):
            if self.which(tool):
                r.ok(f"{tool} found")
            else:
                r.fail(f"{tool} not found - the gcc-arm-none-eabi package should provide this; check your install")
        if self.which("git"):
            r.ok(f"git found: {self._first_line(['git', '--version'])}")
        else:
            r.fail("git not found - needed to fetch ST's own HAL/CMSIS sources.")
            self.stop()
        r.ok(f"python3 found: Python {platform.python_version()}")

    @staticmethod
    def _first_line(command: list[str]) -> str:
        try:
            text = subprocess.run(command, capture_output=True, text=True, check=False).stdout
        except OSError:
            return ""
        return text.splitlines()[0] if text else ""

    # -- stage 2 ---------------------------------------------------------------------------------------------------

    def compile_stage(self, target: str = "all") -> None:
        Stage2(self, target).run_all()

    # -- stage 1b --------------------------------------------------------------------------------------------------

    def host_tests(self) -> None:
        r = self.report
        r.step("1b. Host logic tests (plain gcc, not arm-none-eabi-gcc)")
        if self.env.get("HYDRA_UMC_SKIP_HOST_TESTS", "0") == "1":
            r.warn("host logic tests skipped (HYDRA_UMC_SKIP_HOST_TESTS=1)")
            return
        cc = self.host_compiler()
        if cc is None:
            r.warn("no host C compiler (cc/gcc) found - skipping host logic tests")
            return
        root = self.root
        build = f"{root}/build"
        os.makedirs(build, exist_ok=True)
        tweetnacl_object = f"{build}/host_tests_tweetnacl.o"

        def run_case(case: Case) -> None:
            for compile_ in case.compiles:
                command = compile_.command(root, cc, tweetnacl_object)
                if self.run(command, False) != 0:
                    r.fail(case.fail_compile or f"{case.ok.split(' host ')[0]} host test suite failed to compile")
                    self.stop()
                if case.compiled and compile_ is case.compiles[0] and len(case.compiles) == 1:
                    r.ok(case.compiled)
                if self.run([f"{build}/{compile_.binary}"], compile_.quiet) != 0:
                    if case.rerun_on_failure:
                        self.run([f"{build}/{compile_.binary}"], False)
                    r.fail(case.fail or case.ok.replace(" passed", " FAILED", 1))
                    self.stop()
            r.ok(case.ok)

        cases = host_cases(root)
        # the first case reports its compilation on a line of its own, with the name of the compiler
        first = cases[0]
        command = first.compiles[0].command(root, cc, tweetnacl_object)
        if self.run(command, False) != 0:
            r.fail("host test suite failed to compile")
            self.stop()
        r.ok(f"host test suite compiled ({cc} -std=c11 -Wall -Wextra)")
        if self.run([f"{build}/{first.compiles[0].binary}"], False) != 0:
            r.fail("relay_rx_queue host logic tests FAILED")
            self.stop()
        r.ok(first.ok)
        for case in cases[1:]:
            run_case(case)
        # the vendored TweetNaCl is compiled once, on its own, with its warnings off (third-party text that is not edited for style)
        tweetnacl = [cc, "-std=gnu11", "-w", "-c", f"{root}/{G474_BOOT}/tweetnacl.c", "-o", tweetnacl_object]
        if self.run(tweetnacl, False) != 0:
            r.fail("the vendored TweetNaCl does not compile on the host")
            self.stop()
        for case in bootloader_cases(root):
            run_case(case)
        for message, command in protocol_commands(root, cc):
            if self.run(command, False) != 0:
                r.fail(message.replace("compiles against", "does not compile against"))
                self.stop()
            r.ok(message)
        # static source guards (watchdog window, hand-shifted FDCAN DLC) and the version-bump helper, plain python3 unittest
        if self.which("python3"):
            unittest = ["python3", "-m", "unittest", "discover", "-s", f"{root}/tests", "-p", "test_*.py"]
            if self.run(unittest, True) != 0:
                self.run(unittest, False)
                r.fail("python source-guard and version-bump tests FAILED")
                self.stop()
            r.ok("python source-guard and version-bump tests passed")


def main(argv: Sequence[str] | None = None, runner: Runner = default_runner) -> int:
    parser = argparse.ArgumentParser(description="The firmware build (stage 1: the toolchain check and the logic tests of the host).")
    parser.add_argument("target", nargs="?", default="all")
    parser.add_argument("--clean", action="store_true", help="remove the build folder first")
    parser.add_argument("--host-tests-only", action="store_true", help="run the logic tests of the host and stop")
    parser.add_argument("--compile-only", action="store_true", help="run the compilation and linking of the boards (steps 2 to 10b) and stop")
    parser.add_argument("--root", default=str(Path(__file__).resolve().parent.parent), help="the repository (default: the one this script is in)")
    args = parser.parse_args(argv)
    root = Path(args.root)
    report = Report()
    print("Building does not change any version. Raise one explicitly with: python3 bump_firmware_versions.py")
    build = Build(root, report, runner)
    try:
        if args.clean:
            print(f"Removing {root / 'build'} ...")
            shutil.rmtree(root / "build", ignore_errors=True)
        if args.target not in ("all", "g474", "h745"):
            print(f"unknown target {args.target!r}: use all, g474 or h745")
            return 1
        if not args.host_tests_only and not args.compile_only:
            build.toolchain()
        if not args.compile_only:
            build.host_tests()
        if args.host_tests_only:
            report.summary()
            return 0 if report.failed == 0 else 1
        build.compile_stage(args.target)
        if args.compile_only:
            report.summary()
            return 0 if report.failed == 0 else 1
        report.out("")
        report.out("The stages after the compilation and linking (the move into firmware/ and the manifest) are not ported to this script yet: run build_firmware.sh.")
        report.summary()
        return 2 if report.failed == 0 else 1
    except BuildStopped:
        return 1
    finally:
        if sys.stdin is not None and sys.stdin.isatty():
            try:
                input("\nPress Enter to close this window...")
            except EOFError:
                pass


if __name__ == "__main__":
    sys.exit(main())
