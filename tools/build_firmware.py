#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/build_firmware.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
#
# The firmware build as one script that runs the same everywhere, to replace build_firmware.sh and build_firmware.bat. It is ported one stage at a
# time and checked against the shell script at every stage (tests/test_build_firmware_port.py runs both with a stand-in compiler and compares the
# commands they run and the lines they print):
#
#   stage 1   the toolchain check and the logic tests of the host (plain gcc)                    <- this file
#   stage 2+  the compilation, the linking, the packaging and the manifest                       not ported yet: use build_firmware.sh
#
# Building does not change any version. Raise one explicitly with: python3 bump_firmware_versions.py
#
#   python3 tools/build_firmware.py --host-tests-only        runs stage 1 and stops
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


class Build:
    def __init__(self, root: Path, report: Report, runner: Runner = default_runner, which: Callable[[str], str | None] = shutil.which, env: dict | None = None) -> None:
        self.root = str(root).replace("\\", "/")
        self.report = report
        self.run = runner
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
        if not args.host_tests_only:
            build.toolchain()
        build.host_tests()
        if not args.host_tests_only:
            report.out("")
            report.out("The stages after the logic tests of the host (compilation, linking, packaging, manifest) are not ported to this script yet: run build_firmware.sh.")
            report.summary()
            return 2 if report.failed == 0 else 1
        report.summary()
        return 0 if report.failed == 0 else 1
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
