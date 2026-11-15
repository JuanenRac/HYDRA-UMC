/* =============================================================================
 * HYDRA-UMC Firmware - tests/test_usb_host_plan.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Host test of the numbers of the USB host (src/mcu_stm32h757/Common/usb_host_plan.h): the starting values pass, every number out of range is refused with its own code, the edges are
 * accepted, and the FIFO words that the HAL sets up fit the 320 words of the controller. Evidence: simulator.
 *
 *   cc -std=c11 -Wall -Wextra -Isrc/mcu_stm32h757/Common -o test_usb_host_plan tests/test_usb_host_plan.c
 */
#include <stdio.h>

#include "usb_host_plan.h"

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } } while (0)

static UsbpConfig base(void)
{
    UsbpConfig c = {8u, 1u, 0u, 5u};
    return c;
}

int main(void)
{
    UsbpConfig c = base();
    CHECK(UsbPlan_Check(&c) == USBP_OK, "the starting values");
    c.host_channels = 1u; c.vbus_sensing = 0u; c.sof_output = 1u; c.irq_priority = 0u;
    CHECK(UsbPlan_Check(&c) == USBP_OK, "the lowest edges");
    c.host_channels = 12u; c.irq_priority = 15u; c.vbus_sensing = 1u;
    CHECK(UsbPlan_Check(&c) == USBP_OK, "the highest edges");
    c = base(); c.host_channels = 0u;  CHECK(UsbPlan_Check(&c) == USBP_E_CHANNELS, "no channels");
    c = base(); c.host_channels = 13u; CHECK(UsbPlan_Check(&c) == USBP_E_CHANNELS, "13 channels: the controller has 12");
    c = base(); c.vbus_sensing = 2u;   CHECK(UsbPlan_Check(&c) == USBP_E_FLAG, "vbus_sensing 2");
    c = base(); c.sof_output = 2u;     CHECK(UsbPlan_Check(&c) == USBP_E_FLAG, "sof_output 2");
    c = base(); c.irq_priority = 16u;  CHECK(UsbPlan_Check(&c) == USBP_E_PRIORITY, "priority 16");
    CHECK(UsbPlan_Check(NULL) == USBP_E_ARG, "no configuration");
    CHECK(UsbPlan_FifoWordsUsed() == 288u && UsbPlan_FifoWordsUsed() <= USBP_FIFO_TOTAL_WORDS, "288 of the 320 words of the FIFOs");

    if (failures) {
        printf("usb_host_plan host tests: %d FAILED\n", failures);
        return 1;
    }
    printf("usb_host_plan host logic tests passed\n");
    return 0;
}
