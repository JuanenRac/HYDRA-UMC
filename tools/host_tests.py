#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/host_tests.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Compile and run the C host tests of the firmware on Linux, macOS and Windows.

    python tools/host_tests.py [--require] [--list]

The tests build the real firmware sources (bootloaders, relay logic) for the PC with whatever C compiler is there: HOST_CC, or cc, gcc,
clang. Nothing is compiled for a board and nothing is flashed. With no host compiler the run says so and exits 0, unless --require is
given (a release gate wants the tests to have run). Binaries go to build/host_tests/, which the build folder rules already ignore.

The table below is the same list build_firmware.sh runs; tests/test_host_tests_table.py keeps the two from drifting apart.
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
from local_build import local_build_refusal  # noqa: E402  (the lock on builds that compile on this PC)
OUT = ROOT / "build" / "host_tests"
G4 = "src/mcu_stm32g474"
H7 = "src/mcu_stm32h757"
BOOT_DIRS = (f"{G4}/boot", f"{H7}/CM4/boot", f"{H7}/CM7/boot")

STRICT = ("-std=c11", "-Wall", "-Wextra")
GNU = ("-std=gnu11", "-Wall", "-Wextra", "-Wno-int-to-pointer-cast")


@dataclass(frozen=True)
class HostTest:
    name: str
    source: str
    includes: tuple[str, ...]
    flags: tuple[str, ...] = STRICT
    extra_sources: tuple[str, ...] = ()
    defines: tuple[str, ...] = ()
    needs_tweetnacl: bool = False
    syntax_only: bool = False


def table() -> list[HostTest]:
    tests = [HostTest("relay_rx_queue", "tests/test_relay_rx_queue.c", ("tests", "src"))]
    for directory in (G4, f"{H7}/CM4"):
        tests.append(HostTest(f"dlc@{directory}", "tests/test_dlc_to_fdcan_data_length.c", (directory,)))
    for directory in BOOT_DIRS:
        tests.append(HostTest(f"boot_decision@{directory}", "tests/test_boot_decision.c", (directory,)))
    tests += [
        HostTest("flash_geometry", "tests/test_flash_geometry.c", (f"{G4}/boot",)),
        HostTest("relay_send_reassembly", "tests/test_relay_send_reassembly.c", (G4,)),
        HostTest("relay_ids", "tests/test_relay_ids.c", (G4,)),
        HostTest("spi_frame_crc", "tests/test_spi_frame_crc.c", (f"{H7}/CM4/boot",)),
        HostTest("spi_boot_master", "tests/test_spi_boot_master.c", (f"{H7}/Common", "tests")),
        HostTest("spi_boot_master_fsm", "tests/test_spi_boot_master_fsm.c", (f"{H7}/Common", "tests")),
        HostTest("clock_tree@h757", "tests/test_clock_tree.c", (f"{H7}/Common",), defines=("HYDRA_PLATFORM_H757",)),
        HostTest("ipc_link@h745_crc0", "tests/test_ipc_link.c", (f"{H7}/Common",)),
        HostTest("ipc_link@h745_crc1", "tests/test_ipc_link.c", (f"{H7}/Common",), defines=("HYDRA_IPC_REQUIRE_CRC=1",)),
        HostTest("ipc_link@h757_crc0", "tests/test_ipc_link.c", (f"{H7}/Common",), defines=("HYDRA_PLATFORM_H757",)),
        HostTest("ipc_link@h757_crc1", "tests/test_ipc_link.c", (f"{H7}/Common",), defines=("HYDRA_PLATFORM_H757", "HYDRA_IPC_REQUIRE_CRC=1",)),
        HostTest("clock_config@h757", "tests/test_clock_config.c", (f"{H7}/Common", "tests"), defines=("HYDRA_PLATFORM_H757",)),
        HostTest("nor_flash@h757", "tests/test_nor_flash.c", (f"{H7}/Common",), defines=("HYDRA_PLATFORM_H757",)),
        HostTest("memtest", "tests/test_memtest.c", (f"{H7}/Common",)),
        HostTest("sdram_init@both_banks", "tests/test_sdram_init.c", (f"{H7}/Common", "tests"), defines=("HYDRA_PLATFORM_H757",)),
        HostTest("sdram_init@bank2_off", "tests/test_sdram_init.c", (f"{H7}/Common", "tests"), defines=("HYDRA_PLATFORM_H757", "HYDRA_SDRAM_BANK2_ENABLED=0")),
        HostTest("sdram_init@bank1_off", "tests/test_sdram_init.c", (f"{H7}/Common", "tests"), defines=("HYDRA_PLATFORM_H757", "HYDRA_SDRAM_BANK1_ENABLED=0")),
        HostTest("ipc_mailbox", "tests/test_ipc_mailbox.c", (f"{H7}/Common", f"{H7}/CM4/boot")),
        HostTest("ipc_mailbox_require_crc", "tests/test_ipc_mailbox.c", (f"{H7}/Common", f"{H7}/CM4/boot"), defines=("HYDRA_IPC_REQUIRE_CRC=1",)),
        HostTest("app_support", "tests/test_app_support.c", (G4,), defines=("HYDRA_FDCAN_KERNEL_CLOCK_EXPLICIT=0",)),
        HostTest("app_support_switches_on", "tests/test_app_support.c", (G4,),
                 defines=("HYDRA_FDCAN_KERNEL_CLOCK_EXPLICIT=1", "HYDRA_APP_ENTER_BOOTLOADER=1", "HYDRA_HEALTH_FRAME=1", "HYDRA_BOOT_REPORT=1", "HYDRA_INIT_FAULT_LATCH=1")),
        HostTest("spi_system", "tests/test_spi_system.c", (f"{H7}/CM4",)),
        HostTest("spi_app@off", "tests/test_spi_app.c", (f"{H7}/CM4", f"{H7}/CM4/boot"), defines=("SPI_APP_TEST_SWITCHES=0",)),
        HostTest("spi_app@on", "tests/test_spi_app.c", (f"{H7}/CM4", f"{H7}/CM4/boot"), defines=("SPI_APP_TEST_SWITCHES=1",)),
        HostTest("rcc_owner", "tests/test_rcc_owner.c", (f"{H7}/CM4",)),
        HostTest("flash_layer@g474", "tests/test_flash_layer.c", (f"{G4}/boot",), defines=("HYDRA_TEST_ADOPTION",)),
        HostTest("flash_layer@cm4", "tests/test_flash_layer.c", (f"{H7}/CM4/boot",)),
        HostTest("flash_layer@cm7", "tests/test_flash_layer.c", (f"{H7}/CM7/boot",)),
        HostTest("estop_can@g474", "tests/test_estop_can.c", ("tests", G4)),
        HostTest("estop_can@cm4_brain", "tests/test_estop_can.c", ("tests", f"{H7}/CM4"), STRICT, defines=("ESTOP_TEST_BRAIN",)),
        HostTest("estop_input", "tests/test_estop_input.c", (G4,)),
        HostTest("slot_scan_node", "tests/test_slot_scan_node.c", ("tests", f"{G4}/boot")),
        HostTest("fram_node@off", "tests/test_fram_node.c", (f"{H7}/CM4",)),
        HostTest("fram_node@on", "tests/test_fram_node.c", (f"{H7}/CM4",), defines=("HYDRA_FRAM_ERASE=1", "FRAM_TEST_SWITCH=1")),
        HostTest("fram_i2c", "tests/test_fram_i2c.c", (f"{H7}/CM4",)),
        HostTest("slot_scan_master", "tests/test_slot_scan_master.c", ("tests", f"{G4}/boot", f"{H7}/CM4"), STRICT),
        HostTest("slot_scan_sweep", "tests/test_slot_scan_sweep.c", ("tests", f"{H7}/CM4"), STRICT),
        HostTest("bootloader_g474", "tests/test_bootloader_g474_host.c", ("tests/stubs", f"{G4}/boot", "tests"), GNU,
                 (f"{G4}/boot/update_signature.c",), needs_tweetnacl=True),
        HostTest("bootloader_g474_flash_layer", "tests/test_bootloader_g474_host.c", ("tests/stubs", f"{G4}/boot", "tests"), GNU,
                 (f"{G4}/boot/update_signature.c",), defines=("HYDRA_FLASH_LAYER=1",), needs_tweetnacl=True),
    ]
    for core in ("CM4", "CM7"):
        tests.append(HostTest(f"bootloader_h745_{core}", "tests/test_bootloader_h745_host.c", ("tests/stubs", f"{H7}/{core}/boot"), GNU,
                              defines=(f"HYDRA_TEST_{core}",)))
        tests.append(HostTest(f"bootloader_h745_{core}_flash_layer", "tests/test_bootloader_h745_host.c", ("tests/stubs", f"{H7}/{core}/boot"), GNU,
                              defines=(f"HYDRA_TEST_{core}", "HYDRA_FLASH_LAYER=1")))
    for directory in BOOT_DIRS:
        for name in ("test_image_header", "test_update_verify", "test_format2_decision"):
            tests.append(HostTest(f"{name}@{directory}", f"tests/{name}.c", (directory, "tests"), STRICT + ("-Werror",)))
    for directory in BOOT_DIRS:
        for flavour, define in (("trust", "HYDRA_TRUST_DEV"), ("trust_release", "HYDRA_TEST_RELEASE")):
            defines = (define,)
            tests.append(HostTest(f"{flavour}@{directory}", "tests/test_bootloader_trust.c", (directory, "tests"), STRICT,
                                  (f"{directory}/bootloader_trust.c",), defines, needs_tweetnacl=True))
    tests.append(HostTest("bootloader_g474_update", "tests/test_bootloader_g474_update_host.c",
                          ("tests/stubs/protocol", G4, f"{G4}/boot", "tests"), GNU, (f"{G4}/boot/update_signature.c",),
                          needs_tweetnacl=True))
    tests.append(HostTest("bootloader_g474_update_flash_layer", "tests/test_bootloader_g474_update_host.c",
                          ("tests/stubs/protocol", G4, f"{G4}/boot", "tests"), GNU, (f"{G4}/boot/update_signature.c",),
                          defines=("HYDRA_FLASH_LAYER=1",), needs_tweetnacl=True))
    for core in ("CM4", "CM7"):
        tests.append(HostTest(f"bootloader_h745_{core}_protocol", f"{H7}/{core}/boot/bootloader_protocol.c",
                              ("tests/stubs/h7protocol", f"{H7}/{core}/boot", f"{H7}/Common", f"{H7}/{core}", H7, "tests"),
                              ("-std=gnu11", "-Wall", "-Wno-int-to-pointer-cast"), syntax_only=True))
    return tests


def find_compiler() -> str | None:
    """HOST_CC, or the first of cc, gcc, clang that is on the PATH (None when there is none, or when compiling on this PC is locked: see local_build.py)."""
    if local_build_refusal() is not None:
        return None
    wanted = os.environ.get("HOST_CC")
    for candidate in ([wanted] if wanted else []) + ["cc", "gcc", "clang"]:
        found = shutil.which(candidate)
        if found:
            return found
    return None


def _safe(name: str) -> str:
    return "".join(ch if ch.isalnum() else "_" for ch in name)


def build_command(compiler: str, test: HostTest, output: Path, tweetnacl: Path | None) -> list[str]:
    command = [compiler, *test.flags]
    command += [f"-D{define}" for define in test.defines]
    command += [f"-I{ROOT / include}" for include in test.includes]
    if test.syntax_only:
        return command + ["-fsyntax-only", str(ROOT / test.source)]
    command += ["-o", str(output), str(ROOT / test.source)]
    if test.needs_tweetnacl and tweetnacl is not None:
        command.append(str(tweetnacl))
    command += [str(ROOT / extra) for extra in test.extra_sources]
    return command


def run_all(compiler: str) -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    suffix = ".exe" if os.name == "nt" else ""
    tweetnacl = OUT / "tweetnacl.o"
    # TweetNaCl is third-party text that is not edited for style: compiled on its own with the warnings off.
    built = subprocess.run([compiler, "-std=gnu11", "-w", "-c", str(ROOT / G4 / "boot" / "tweetnacl.c"), "-o", str(tweetnacl)])
    failures = 0 if built.returncode == 0 else 1
    if failures:
        print("FAIL tweetnacl does not compile")
    for test in table():
        binary = OUT / (_safe(test.name) + suffix)
        compiled = subprocess.run(build_command(compiler, test, binary, tweetnacl))
        if compiled.returncode != 0:
            print(f"FAIL {test.name}: does not compile")
            failures += 1
            continue
        if test.syntax_only:
            print(f"PASS {test.name} (syntax)")
            continue
        ran = subprocess.run([str(binary)], stdout=subprocess.DEVNULL)
        print(f"{'PASS' if ran.returncode == 0 else 'FAIL'} {test.name}")
        failures += ran.returncode != 0
    print("HOST_TESTS=" + ("PASS" if failures == 0 else f"FAIL ({failures})"))
    return 1 if failures else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Compile and run the C host tests of the firmware.")
    parser.add_argument("--require", action="store_true", help="fail when there is no host C compiler")
    parser.add_argument("--list", action="store_true", help="print the tests and build nothing")
    options = parser.parse_args(argv)
    if options.list:
        for test in table():
            print(test.name, test.source)
        return 0
    compiler = find_compiler()
    if compiler is None:
        print("HOST_TESTS=SKIPPED " + (local_build_refusal() or "no host C compiler (cc/gcc/clang) found; set HOST_CC"))
        return 1 if options.require else 0
    return run_all(compiler)


if __name__ == "__main__":
    sys.exit(main())
