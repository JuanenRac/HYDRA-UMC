# =============================================================================
# HYDRA-UMC - tests/test_fdcan_plan.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""The start-up of FDCAN1 on the CM7: the Python twin (tools/fdcan_plan.py) gives the numbers that tests/test_fdcan_plan.c checks in C, config/fdcan.cfg is the same as the starting
values compiled into Common/fdcan_plan.h, the timing is the one of the platform profile, the filters are the two of the CM4, the switches ship off, and the new sources are in both
runners with floors. Evidence: simulator (the peripheral double runs in tests/test_fdcan1_start.c and tests/test_gateway_self.c)."""
import json
import re
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import fdcan_plan as fp  # noqa: E402

PLAN_H = (ROOT / "src/mcu_stm32h757/Common/fdcan_plan.h").read_text(encoding="utf-8")
SELF_H = (ROOT / "src/mcu_stm32h757/Common/gateway_self.h").read_text(encoding="utf-8")
START_C = (ROOT / "src/mcu_stm32h757/CM7/Fdcan1Start.c").read_text(encoding="utf-8")
START_H = (ROOT / "src/mcu_stm32h757/CM7/Fdcan1Start.h").read_text(encoding="utf-8")
CM4_MAIN = (ROOT / "src/mcu_stm32h757/CM4/boot/bootloader_main.c").read_text(encoding="utf-8")
CM4_PROTOCOL = (ROOT / "src/mcu_stm32h757/CM4/boot/bootloader_protocol.c").read_text(encoding="utf-8")
PROFILE = (ROOT / "src/mcu_stm32h757/Common/platform_profile.h").read_text(encoding="utf-8")
BOOT_IDS = (ROOT / "firmware_base/boot/boot_protocol_ids.h").read_text(encoding="utf-8")


def cfg(**changes):
    base = fp.load_config()
    base.update(changes)
    return base


def macro(text, name):
    return int(re.search(rf"#define {name} (0x[0-9A-Fa-f]+|\d+)UL", text).group(1), 0)


class TwinCases(unittest.TestCase):
    """The same numbers as tests/test_fdcan_plan.c."""

    def test_the_defaults_are_the_profile(self):
        plan = fp.make_plan(fp.load_config())
        self.assertEqual((plan["prescaler"], plan["seg1"], plan["seg2"]), (5, 16, 3))
        self.assertEqual((plan["sjw"], plan["actual_bps"], plan["actual_sample_permille"]), (3, 1_000_000, 850))
        self.assertEqual(plan["filters"], [(0x600, 0x600), (0x001, 0x7FF)])
        self.assertEqual((plan["rx_elements"], plan["tx_elements"]), (8, 8))

    def test_one_filter_without_the_emergency_stop(self):
        self.assertEqual(fp.make_plan(cfg(estop_filter=0))["filters"], [(0x600, 0x600)])

    def test_other_clocks(self):
        plan = fp.make_plan(cfg(kernel_hz=80_000_000, bit_rate_bps=500_000))
        self.assertEqual((plan["prescaler"], plan["seg1"], plan["seg2"]), (8, 16, 3))
        plan = fp.make_plan(cfg(kernel_hz=48_000_000, max_quanta=16))
        self.assertEqual((plan["prescaler"], plan["seg1"], plan["seg2"], plan["actual_sample_permille"]), (3, 13, 2, 875))
        plan = fp.make_plan(cfg(kernel_hz=40_000_000, bit_rate_bps=125_000))
        self.assertEqual(plan["prescaler"], 16)
        self.assertEqual(plan["seg1"] + plan["seg2"] + 1, 20)
        plan = fp.make_plan(cfg(kernel_hz=50_000_000))
        self.assertEqual((plan["prescaler"], plan["seg1"] + plan["seg2"] + 1), (5, 10))

    def test_segment_2_is_never_empty(self):
        plan = fp.make_plan(cfg(kernel_hz=8_000_000, sample_permille=950))
        self.assertEqual((plan["prescaler"], plan["seg1"], plan["seg2"], plan["sjw"]), (1, 6, 1, 1))

    def test_no_timing(self):
        for changes in ({"kernel_hz": 7_000_000}, {"kernel_hz": 100_000_007}, {"kernel_hz": 200_000_000, "bit_rate_bps": 10_000, "max_quanta": 8}):
            with self.assertRaises(ValueError, msg=str(changes)):
                fp.make_plan(cfg(**changes))

    def test_the_lowest_edges(self):
        plan = fp.make_plan(cfg(kernel_hz=1_000_000, bit_rate_bps=10_000, max_quanta=100, sjw=1, rx_elements=32, tx_elements=1, sample_permille=500))
        self.assertEqual((plan["prescaler"], plan["seg1"] + plan["seg2"] + 1, plan["rx_elements"], plan["tx_elements"]), (1, 100, 32, 1))

    def test_every_number_out_of_range_is_refused(self):
        for key, (low, high) in fp.LIMITS.items():
            for bad in (low - 1, high + 1):
                with self.assertRaises(ValueError, msg=f"{key} {bad}"):
                    fp.make_plan(cfg(**{key: bad}))

    def test_the_same_cases_are_in_the_c_test(self):
        source = (ROOT / "tests/test_fdcan_plan.c").read_text(encoding="utf-8")
        for needle in ("48000000UL", "80000000UL", "40000000UL", "50000000UL", "8000000UL", "100000007UL", "p.seg2 == 3u", "p.actual_sample_permille == 875u"):
            self.assertIn(needle, source)


class ProfileAndCm4(unittest.TestCase):
    def test_the_timing_is_the_one_of_the_h757_profile(self):
        h757 = PROFILE[PROFILE.index("#elif defined(HYDRA_PLATFORM_H757)"):]
        values = {name: int(number.rstrip("UL")) for name, number in re.findall(r"#define (HYDRA_STACK_BUS_\w+) (\w+)", h757)}
        plan = fp.make_plan(fp.load_config())
        self.assertEqual(plan["prescaler"], values["HYDRA_STACK_BUS_PRESCALER"])
        self.assertEqual(plan["seg1"], values["HYDRA_STACK_BUS_TIME_SEG1"])
        self.assertEqual(plan["seg2"], values["HYDRA_STACK_BUS_TIME_SEG2"])
        self.assertEqual(fp.load_config()["kernel_hz"], values["HYDRA_STACK_BUS_KERNEL_CLOCK_HZ"])
        self.assertEqual(fp.load_config()["bit_rate_bps"], values["HYDRA_STACK_BUS_BIT_RATE_BPS"])
        # the profile sets a jump width of 4 with a segment 2 of 3: the plan holds it to segment 2 (a finding for the bench, not changed in the sources of the CM4)
        self.assertEqual(values["HYDRA_STACK_BUS_SYNC_JUMP_WIDTH"], 4)
        self.assertEqual(plan["sjw"], 3)

    def test_the_filters_are_the_ones_of_the_cm4(self):
        ids = {name: int(value, 16) for name, value in re.findall(r"#define (BOOT_\w+)\s+(0x[0-9A-Fa-f]+)u", CM4_MAIN)}
        self.assertEqual((ids["BOOT_STACKA_FILTER_ID"], ids["BOOT_STACKA_FILTER_MASK"]), fp.STACKA_FILTER)
        self.assertEqual(ids["BOOT_ESTOP_CAN_ID"], fp.ESTOP_FILTER[0])
        self.assertEqual(fp.ESTOP_FILTER[1], 0x7FF)
        self.assertEqual((macro(PLAN_H, "FDCP_STACKA_FILTER_ID"), macro(PLAN_H, "FDCP_STACKA_FILTER_MASK")), fp.STACKA_FILTER)
        self.assertEqual((macro(PLAN_H, "FDCP_ESTOP_ID"), macro(PLAN_H, "FDCP_ESTOP_MASK")), fp.ESTOP_FILTER)

    def test_the_error_counter_frames_are_the_ones_of_the_cm4(self):
        ids = {name: int(value, 16) for name, value in re.findall(r"#define (OFS_[A-Z_0-9]+)\s+(0x[0-9A-Fa-f]+)", BOOT_IDS)}
        self.assertEqual(ids["OFS_QUERY_ERROR_COUNTERS"], 0x0B)
        self.assertEqual(ids["OFS_ERROR_COUNTERS_RESPONSE"], 0x0C)
        self.assertIn("#define GSELF_QUERY_ERROR_COUNTERS 0x0Bu", SELF_H)
        self.assertIn("#define GSELF_ERROR_COUNTERS_RESPONSE 0x0Cu", SELF_H)
        self.assertIn("(hfdcan1.Instance->ECR >> 8) & 0x7Fu", CM4_MAIN + CM4_PROTOCOL)  # the receive counter is 7 bits there too
        self.assertIn("#define GSELF_REC_MASK 0x7Fu", SELF_H)


class ConfigAndSources(unittest.TestCase):
    def test_the_file_is_valid_and_is_the_compiled_default(self):
        values = fp.load_config()
        self.assertEqual(
            values,
            {
                "kernel_hz": macro(PLAN_H, "FDCP_DEFAULT_KERNEL_HZ"),
                "bit_rate_bps": macro(PLAN_H, "FDCP_DEFAULT_BIT_RATE_BPS"),
                "sample_permille": macro(PLAN_H, "FDCP_DEFAULT_SAMPLE_PERMILLE"),
                "max_quanta": macro(PLAN_H, "FDCP_DEFAULT_MAX_QUANTA"),
                "sjw": macro(PLAN_H, "FDCP_DEFAULT_SJW"),
                "rx_elements": macro(PLAN_H, "FDCP_DEFAULT_RX_ELEMENTS"),
                "tx_elements": macro(PLAN_H, "FDCP_DEFAULT_TX_ELEMENTS"),
                "estop_filter": macro(PLAN_H, "FDCP_DEFAULT_ESTOP_FILTER"),
            },
        )

    def test_the_c_code_refuses_the_same_limits(self):
        for key, (low, high) in fp.LIMITS.items():
            prefix = {"kernel_hz": "FDCP_KERNEL", "bit_rate_bps": "FDCP_BIT_RATE", "sample_permille": "FDCP_SAMPLE", "max_quanta": "FDCP_QUANTA", "sjw": "FDCP_SJW"}.get(key, "FDCP_ELEMENTS")
            suffix = {"kernel_hz": ("_MIN_HZ", "_MAX_HZ"), "bit_rate_bps": ("_MIN_BPS", "_MAX_BPS"), "sample_permille": ("_MIN_PERMILLE", "_MAX_PERMILLE")}.get(key, ("_MIN", "_MAX"))
            self.assertEqual((macro(PLAN_H, prefix + suffix[0]), macro(PLAN_H, prefix + suffix[1])), (low, high), key)
        self.assertEqual(macro(PLAN_H, "FDCP_PRESCALER_MAX"), fp.PRESCALER_MAX)
        self.assertEqual(macro(PLAN_H, "FDCP_QUANTA_MIN"), fp.QUANTA_MIN)

    def test_values_out_of_range_in_the_file_are_refused(self):
        base = (ROOT / "config/fdcan.cfg").read_text(encoding="utf-8")
        for good, bad in (("kernel_hz = 100000000", "kernel_hz = 999999"), ("sjw = 4", "sjw = 5"), ("estop = 1", "estop = 2"), ("tx_elements = 8", "tx_elements = 33")):
            with tempfile.TemporaryDirectory() as folder:
                path = Path(folder) / "fdcan.cfg"
                path.write_text(base.replace(good, bad), encoding="utf-8")
                with self.assertRaises(ValueError, msg=bad):
                    fp.load_config(path)

    def test_the_switch_ships_off_and_the_whole_unit_is_inside_it(self):
        self.assertIn("#define HYDRA_FDCAN1_START 0", START_H)
        self.assertTrue(START_C.rstrip().endswith("#endif /* HYDRA_FDCAN1_START */"))
        self.assertEqual(START_C.count("#if HYDRA_FDCAN1_START"), 1)

    def test_the_start_order_is_the_one_of_the_cm4(self):
        order = [START_C.index(call) for call in ("HAL_FDCAN_Init(", "HAL_FDCAN_ConfigFilter(", "HAL_FDCAN_ConfigGlobalFilter(", "HAL_FDCAN_Start(")]
        self.assertEqual(order, sorted(order))
        cm4 = [CM4_MAIN.index(call) for call in ("HAL_FDCAN_Init(", "HAL_FDCAN_ConfigFilter(", "HAL_FDCAN_ConfigGlobalFilter(", "HAL_FDCAN_Start(")]
        self.assertEqual(cm4, sorted(cm4))

    def test_the_new_sources_are_in_both_runners_and_have_floors(self):
        host = (ROOT / "tools/host_tests.py").read_text(encoding="utf-8")
        build = (ROOT / "tools/build_firmware.py").read_text(encoding="utf-8")
        for test in ("tests/test_fdcan_plan.c", "tests/test_gateway_self.c", "tests/test_fdcan1_start.c"):
            self.assertIn(test, host)
        for token in ("test_fdcan_plan", "test_gateway_self", "test_fdcan1_start", "-DHYDRA_FDCAN1_START=1", "Fdcan1Start.c"):
            self.assertIn(token, build)
        floors = json.loads((ROOT / "tools/host_coverage_floor.json").read_text(encoding="utf-8"))["modules"]
        for module in ("Common/fdcan_plan.h", "Common/gateway_self.h", "CM7/Fdcan1Start.c"):
            self.assertEqual(floors["src/mcu_stm32h757/" + module], 100)


class FieldBus(unittest.TestCase):
    """FDCAN2 of the field bus: the same cases as tests/test_fdcan_plan.c and tests/test_fdcan1_start.c."""

    def test_the_second_controller_follows_the_first(self):
        first = fp.make_plan(fp.load_config())
        first["ram_offset"] = 0
        self.assertEqual(fp.ram_words(first), 66)
        field_cfg = fp.load_field_config()
        field = fp.make_field_plan(field_cfg, first)
        self.assertEqual((field["ram_offset"], field["filters"]), (66, [(0, 0)]))
        self.assertEqual((field["prescaler"], field["seg1"], field["seg2"]), (first["prescaler"], first["seg1"], first["seg2"]))
        third = fp.make_field_plan({**field_cfg, "filter_id": 0x123, "filter_mask": 0x7F0}, field)
        self.assertEqual(third["ram_offset"], 66 + 1 + 64)

    def test_no_room_in_the_message_ram(self):
        first = fp.make_plan(fp.load_config())
        first["ram_offset"] = 2500
        with self.assertRaises(ValueError):
            fp.make_field_plan(fp.load_field_config(), first)

    def test_the_file_is_valid_and_takes_the_numbers_of_the_stack_bus(self):
        field, stack = fp.load_field_config(), fp.load_config()
        for key in ("kernel_hz", "bit_rate_bps", "sample_permille", "max_quanta", "sjw", "rx_elements", "tx_elements"):
            self.assertEqual(field[key], stack[key], key)
        base = (ROOT / "config/fdcan2.cfg").read_text(encoding="utf-8")
        for good, bad in (("filter_id = 0x000", "filter_id = 0x800"), ("filter_mask = 0x000", "filter_mask = 0x800"), ("sjw = 4", "sjw = 5")):
            with tempfile.TemporaryDirectory() as folder:
                path = Path(folder) / "fdcan2.cfg"
                path.write_text(base.replace(good, bad), encoding="utf-8")
                with self.assertRaises(ValueError, msg=bad):
                    fp.load_field_config(path)

    def test_the_ram_numbers_and_pins_are_the_same_in_c_and_the_profile(self):
        self.assertEqual((macro(PLAN_H, "FDCP_RAM_WORDS_TOTAL"), macro(PLAN_H, "FDCP_ELEMENT_WORDS")), (fp.RAM_WORDS_TOTAL, fp.ELEMENT_WORDS))
        text = json.dumps(json.loads((ROOT / "docs/platform_profiles.json").read_text(encoding="utf-8")))
        self.assertIn('"peripheral": "FDCAN2"', text)
        self.assertIn("PB12", text)
        self.assertIn("MessageRAMOffset = plan->ram_offset", START_C)


if __name__ == "__main__":
    unittest.main()
