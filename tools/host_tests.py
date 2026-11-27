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

The table below is the same list tools/build_firmware.py runs; tests/test_host_tests_table.py keeps the two from drifting apart.
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
        HostTest("expansion_slots", "tests/test_expansion_slots.c", (f"{H7}/Common", "tests")),
        HostTest("host_link_master", "tests/test_host_link_master.c", (f"{H7}/Common", f"{H7}/CM4/boot", "tests")),
        HostTest("gateway_dispatch@h745", "tests/test_gateway_dispatch.c", (f"{H7}/Common", f"{H7}/CM4/boot", "tests")),
        HostTest("gateway_dispatch@h757", "tests/test_gateway_dispatch.c", (f"{H7}/Common", f"{H7}/CM4/boot", "tests"), defines=("HYDRA_PLATFORM_H757",)),
        HostTest("gateway_stack_a", "tests/test_gateway_stack_a.c", (f"{H7}/Common", f"{H7}/CM4/boot", "tests")),
        HostTest("gateway_fdcan1", "tests/test_gateway_fdcan1.c", ("tests/stubs/h7fdcan", f"{H7}/CM7", f"{H7}/Common"), defines=("HYDRA_GATEWAY_FDCAN1=1",),
                 extra_sources=(f"{H7}/CM7/GatewayFdcan1.c",)),
        HostTest("fdcan_plan@h757", "tests/test_fdcan_plan.c", (f"{H7}/Common",), defines=("HYDRA_PLATFORM_H757",)),
        HostTest("gateway_self", "tests/test_gateway_self.c", (f"{H7}/Common",)),
        HostTest("fdcan1_start", "tests/test_fdcan1_start.c", ("tests/stubs/h7fdcanstart", f"{H7}/CM7", f"{H7}/Common"), defines=("HYDRA_FDCAN1_START=1",),
                 extra_sources=(f"{H7}/CM7/Fdcan1Start.c",)),
        HostTest("spi_boot_master_fsm", "tests/test_spi_boot_master_fsm.c", (f"{H7}/Common", "tests")),
        HostTest("clock_tree@h757", "tests/test_clock_tree.c", (f"{H7}/Common",), defines=("HYDRA_PLATFORM_H757",)),
        HostTest("platform_default@h745", "tests/test_platform_default.c", (f"{H7}/Common",), defines=("EXPECT_PLATFORM=745",)),
        HostTest("platform_default@h745_switch_off", "tests/test_platform_default.c", (f"{H7}/Common",), defines=("EXPECT_PLATFORM=745", "HYDRA_DEFAULT_PLATFORM_H757=0")),
        HostTest("platform_default@h757_switch_on", "tests/test_platform_default.c", (f"{H7}/Common",), defines=("EXPECT_PLATFORM=757", "HYDRA_DEFAULT_PLATFORM_H757=1")),
        HostTest("platform_default@h757_named", "tests/test_platform_default.c", (f"{H7}/Common",), defines=("EXPECT_PLATFORM=757", "HYDRA_PLATFORM_H757")),
        HostTest("platform_default@h745_named_beats_switch", "tests/test_platform_default.c", (f"{H7}/Common",), defines=("EXPECT_PLATFORM=745", "HYDRA_DEFAULT_PLATFORM_H757=1", "HYDRA_PLATFORM_H745")),
        HostTest("clock_config@h757_default_switch", "tests/test_clock_config.c", (f"{H7}/Common", "tests"), defines=("HYDRA_DEFAULT_PLATFORM_H757=1",)),
        HostTest("boot_roles@cm4_h745", "tests/test_boot_roles.c", (f"{H7}/Common",), defines=("CORE_CM4",)),
        HostTest("boot_roles@cm4_h757", "tests/test_boot_roles.c", (f"{H7}/Common",), defines=("CORE_CM4", "HYDRA_PLATFORM_H757")),
        HostTest("boot_roles@cm7_h745", "tests/test_boot_roles.c", (f"{H7}/Common",), defines=("CORE_CM7",)),
        HostTest("boot_roles@cm7_h757", "tests/test_boot_roles.c", (f"{H7}/Common",), defines=("CORE_CM7", "HYDRA_PLATFORM_H757")),
        HostTest("ipc_link@h745_crc0", "tests/test_ipc_link.c", (f"{H7}/Common",)),
        HostTest("ipc_link@h745_crc1", "tests/test_ipc_link.c", (f"{H7}/Common",), defines=("HYDRA_IPC_REQUIRE_CRC=1",)),
        HostTest("ipc_link@h757_crc0", "tests/test_ipc_link.c", (f"{H7}/Common",), defines=("HYDRA_PLATFORM_H757",)),
        HostTest("ipc_link@h757_crc1", "tests/test_ipc_link.c", (f"{H7}/Common",), defines=("HYDRA_PLATFORM_H757", "HYDRA_IPC_REQUIRE_CRC=1",)),
        HostTest("i2c_plan", "tests/test_i2c_plan.c", (f"{H7}/Common",)),
        HostTest("nor_backup_plan", "tests/test_nor_backup_plan.c", (f"{H7}/Common",)),
        HostTest("boot_nor_stage", "tests/test_boot_nor_stage.c", (f"{H7}/Common",)),
        HostTest("nor_spi5_pins", "tests/test_nor_spi5_pins.c", (f"{H7}/Common",)),
        HostTest("nor_update_flow", "tests/test_nor_update_flow.c", (f"{H7}/Common",)),
        HostTest("fram_i2c4", "tests/test_fram_i2c4.c", ("tests/stubs/h7i2c4", f"{H7}/CM7", f"{H7}/CM4", f"{H7}/Common"), defines=("HYDRA_FRAM_I2C4_CM7=1",),
                 extra_sources=(f"{H7}/CM7/FramI2c4.c",)),
        HostTest("i2c_buses", "tests/test_i2c_buses.c", ("tests/stubs/h7i2cbuses", f"{H7}/CM7", f"{H7}/Common"), defines=("HYDRA_I2C_BUSES_CM7=1",),
                 extra_sources=(f"{H7}/CM7/I2cBuses.c",)),
        HostTest("dac_plan", "tests/test_dac_plan.c", (f"{H7}/Common",)),
        HostTest("dac_out", "tests/test_dac_out.c", ("tests/stubs/h7dac", f"{H7}/CM7", f"{H7}/Common",), defines=("HYDRA_DAC_CM7=1",),
                 extra_sources=(f"{H7}/CM7/DacOut.c",)),
        HostTest("usb_host_plan", "tests/test_usb_host_plan.c", (f"{H7}/Common",)),
        HostTest("usb_host_fs", "tests/test_usb_host_fs.c", ("tests/stubs/h7usb", f"{H7}/CM7", f"{H7}/Common",), defines=("HYDRA_USB_HOST_CM7=1",),
                 extra_sources=(f"{H7}/CM7/UsbHostFs.c",)),
        HostTest("dsi_plan", "tests/test_dsi_plan.c", (f"{H7}/Common",)),
        HostTest("panel_start", "tests/test_panel_start.c", (f"{H7}/Common",)),
        HostTest("adc_plan", "tests/test_adc_plan.c", (f"{H7}/Common",)),
        HostTest("adc_reader", "tests/test_adc_reader.c", ("tests/stubs/h7adc", f"{H7}/CM7", f"{H7}/Common",), defines=("HYDRA_ADC_CM7=1",),
                 extra_sources=(f"{H7}/CM7/AdcReader.c",)),
        HostTest("dsi_host", "tests/test_dsi_host.c", ("tests/stubs/h7dsi", f"{H7}/CM7", f"{H7}/Common",), defines=("HYDRA_DSI_HOST=1",),
                 extra_sources=(f"{H7}/CM7/DsiHost.c",)),
        HostTest("ui_flush_plan", "tests/test_ui_flush_plan.c", (f"{H7}/Common",)),
        HostTest("ui_tick_plan", "tests/test_ui_tick_plan.c", (f"{H7}/Common",)),
        HostTest("touch_gt911", "tests/test_touch_gt911.c", (f"{H7}/Common",)),
        HostTest("clock_config@h757", "tests/test_clock_config.c", (f"{H7}/Common", "tests"), defines=("HYDRA_PLATFORM_H757",)),
        HostTest("nor_flash@h757", "tests/test_nor_flash.c", (f"{H7}/Common",), defines=("HYDRA_PLATFORM_H757",)),
        HostTest("nor_backup_glue@h757", "tests/test_nor_backup_glue.c", (f"{H7}/Common",), defines=("HYDRA_PLATFORM_H757",)),
        HostTest("nor_spi5", "tests/test_nor_spi5.c", ("tests/stubs/h7spi5", f"{H7}/CM7", f"{H7}/Common"), defines=("HYDRA_PLATFORM_H757", "HYDRA_NOR_SPI5=1"),
                 extra_sources=(f"{H7}/CM7/NorSpi5.c",)),
        HostTest("tmc_card", "tests/test_tmc_card.c", (f"{H7}/Common",)),
        HostTest("tmc_card_spi6", "tests/test_tmc_card_spi6.c", ("tests/stubs/h7spi6", f"{H7}/CM7", f"{H7}/Common"), defines=("HYDRA_PLATFORM_H757", "HYDRA_TMC_CARD_SPI6=1"),
                 extra_sources=(f"{H7}/CM7/TmcCardSpi6.c",)),
        HostTest("display_ltdc", "tests/test_display_ltdc.c", ("tests/stubs/h7ltdc", f"{H7}/CM7", f"{H7}/Common"), defines=("HYDRA_PLATFORM_H757", "HYDRA_DISPLAY_LTDC=1"),
                 extra_sources=(f"{H7}/CM7/DisplayLtdc.c",)),
        HostTest("memtest", "tests/test_memtest.c", (f"{H7}/Common",)),
        HostTest("sdram_init@bank1_default", "tests/test_sdram_init.c", (f"{H7}/Common", "tests"), defines=("HYDRA_PLATFORM_H757",)),
        HostTest("sdram_init@both_banks", "tests/test_sdram_init.c", (f"{H7}/Common", "tests"), defines=("HYDRA_PLATFORM_H757", "HYDRA_SDRAM_BANK2_ENABLED=1")),
        HostTest("sdram_init@bank1_off", "tests/test_sdram_init.c", (f"{H7}/Common", "tests"), defines=("HYDRA_PLATFORM_H757", "HYDRA_SDRAM_BANK1_ENABLED=0", "HYDRA_SDRAM_BANK2_ENABLED=1")),
        HostTest("sdram_banks@bank1_default", "tests/test_sdram_banks.c", (f"{H7}/Common",), defines=("HYDRA_PLATFORM_H757",)),
        HostTest("sdram_banks@bank2_on_trust", "tests/test_sdram_banks.c", (f"{H7}/Common",), defines=("HYDRA_PLATFORM_H757", "HYDRA_SDRAM_BANK2_ENABLED=1")),
        HostTest("sdram_banks@bank2_probed", "tests/test_sdram_banks.c", (f"{H7}/Common",), defines=("HYDRA_PLATFORM_H757", "HYDRA_SDRAM_BANK2_ENABLED=1", "HYDRA_SDRAM_BANK2_PROBE=1")),
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
        HostTest("estop_can@g474_resync", "tests/test_estop_can.c", ("tests", G4), defines=("ESTOP_RESYNC_ON_NEW_EPOCH=1",)),
        HostTest("estop_can@cm4_brain", "tests/test_estop_can.c", ("tests", f"{H7}/CM4"), STRICT, defines=("ESTOP_TEST_BRAIN",)),
        HostTest("estop_input", "tests/test_estop_input.c", (G4,)),
        HostTest("enabling_device", "tests/test_enabling_device.c", ("tests", f"{H7}/CM4"), STRICT),
        HostTest("zones_frame@on", "tests/test_zones_frame.c", ("tests", f"{H7}/CM4"), STRICT, defines=("HYDRA_ZONES_CAN_FILTER=1",)),
        HostTest("zones_frame@off", "tests/test_zones_frame.c", ("tests", f"{H7}/CM4"), STRICT),
        HostTest("task_watchdog", "tests/test_task_watchdog.c", ("tests", f"{H7}/CM4"), STRICT),
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
    for core in ("CM4", "CM7"):
        tests.append(HostTest(f"bootloader_h745_{core}_flash_ops", "tests/test_bootloader_h745_flash_host.c", ("tests/stubs", f"{H7}/{core}/boot"), GNU,
                              defines=(f"HYDRA_TEST_{core}",)))
        tests.append(HostTest(f"bootloader_h745_{core}_flash_ops_flash_layer", "tests/test_bootloader_h745_flash_host.c", ("tests/stubs", f"{H7}/{core}/boot"), GNU,
                              defines=(f"HYDRA_TEST_{core}", "HYDRA_FLASH_LAYER=1")))
        # the same test with the metadata log in two sectors that take turns (HYDRA_META_AB=1, off as it ships)
        tests.append(HostTest(f"bootloader_h745_{core}_flash_ops_meta_ab", "tests/test_bootloader_h745_flash_host.c", ("tests/stubs", f"{H7}/{core}/boot"), GNU,
                              defines=(f"HYDRA_TEST_{core}", "HYDRA_META_AB=1")))
        tests.append(HostTest(f"bootloader_h745_{core}_flash_ops_flash_layer_meta_ab", "tests/test_bootloader_h745_flash_host.c", ("tests/stubs", f"{H7}/{core}/boot"), GNU,
                              defines=(f"HYDRA_TEST_{core}", "HYDRA_FLASH_LAYER=1", "HYDRA_META_AB=1")))
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
    for gate in (0, 1):
        tests.append(HostTest(f"bootloader_g474_readback@gate{gate}", "tests/test_bootloader_g474_readback_host.c",
                              ("tests/stubs/protocol", G4, f"{G4}/boot", "tests"), GNU, (f"{G4}/boot/update_signature.c",),
                              defines=(f"HYDRA_READBACK_GATE={gate}",), needs_tweetnacl=True))
    for core in ("CM4", "CM7"):
        tests.append(HostTest(f"bootloader_h745_{core}_protocol", f"{H7}/{core}/boot/bootloader_protocol.c",
                              ("tests/stubs/h7protocol", f"{H7}/{core}/boot", f"{H7}/Common", f"{H7}/{core}", H7, "tests"),
                              ("-std=gnu11", "-Wall", "-Wno-int-to-pointer-cast"), syntax_only=True))
        # the same file with the read-out gate on: the code behind HYDRA_READBACK_GATE is only compiled with the switch at 1
        tests.append(HostTest(f"bootloader_h745_{core}_protocol@gate1", f"{H7}/{core}/boot/bootloader_protocol.c",
                              ("tests/stubs/h7protocol", f"{H7}/{core}/boot", f"{H7}/Common", f"{H7}/{core}", H7, "tests"),
                              ("-std=gnu11", "-Wall", "-Wno-int-to-pointer-cast"), defines=("HYDRA_READBACK_GATE=1",), syntax_only=True))
        for gate in (0, 1):
            tests.append(HostTest(f"readout_gate@{core}@gate{gate}", "tests/test_readout_gate_h7.c", (f"{H7}/{core}/boot",), defines=(f"HYDRA_READBACK_GATE={gate}",)))
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
