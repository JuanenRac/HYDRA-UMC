/* =============================================================================
 * HYDRA-UMC Firmware - tests/test_usb_host_fs.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Host test of the USB host of the CM7 (src/mcu_stm32h757/CM7/UsbHostFs.c) against a double of the HAL (tests/stubs/h7usb): the pins (PA11 and PA12 on alternate function 10, PA9 only
 * with the VBUS sense on), the clock, the numbers handed to the HAL (embedded PHY, full speed, no DMA, no low-power mode, the channels and the flags of the configuration), the order
 * (initialise, interrupt on, start), the undoing of a refused initialisation or start, the refusal of a second open and of a close without an open, the close, the handle for the
 * class drivers, and the interrupt handler that hands the HAL its handle. Evidence: simulator.
 *
 *   cc -std=c11 -Wall -Wextra -DHYDRA_USB_HOST_CM7=1 -Itests/stubs/h7usb -Isrc/mcu_stm32h757/CM7 -Isrc/mcu_stm32h757/Common \
 *      -o test_usb_host_fs tests/test_usb_host_fs.c src/mcu_stm32h757/CM7/UsbHostFs.c
 */
#include <stdio.h>
#include <string.h>

#include "UsbHostFs.h"

#if !HYDRA_USB_HOST_CM7
#error "this test is for the port with its switch on: build it with -DHYDRA_USB_HOST_CM7=1"
#endif

void OTG_FS_IRQHandler(void);

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } } while (0)

GPIO_TypeDef stub_gpioa;
USB_OTG_GlobalTypeDef stub_otg_fs;

static char calls[16];
static unsigned call_count, gpio_count, irq_handled;
static int clock_gpio, clock_usb_on, clock_usb_off;
static GPIO_InitTypeDef gpio_seen[2];
static GPIO_TypeDef *gpio_port[2];
static HCD_InitTypeDef init_seen;
static USB_OTG_GlobalTypeDef *instance_seen;
static uint32_t priority_seen;
static HCD_HandleTypeDef *irq_handle;
static int fail_init, fail_start, fail_stop, fail_deinit;

static void note(char c) { if (call_count < sizeof calls - 1u) calls[call_count++] = c; }

void stub_clock(int which)
{
    if (which == 1) clock_gpio++;
    else if (which == 2) clock_usb_on++;
    else if (which == -2) clock_usb_off++;
}
void HAL_GPIO_Init(GPIO_TypeDef *port, GPIO_InitTypeDef *init) { if (gpio_count < 2u) { gpio_port[gpio_count] = port; gpio_seen[gpio_count] = *init; } gpio_count++; }
void HAL_NVIC_SetPriority(IRQn_Type irq, uint32_t preempt, uint32_t sub) { (void)irq; (void)sub; priority_seen = preempt; note('P'); }
void HAL_NVIC_EnableIRQ(IRQn_Type irq) { (void)irq; note('E'); }
void HAL_NVIC_DisableIRQ(IRQn_Type irq) { (void)irq; note('D'); }
HAL_StatusTypeDef HAL_HCD_Init(HCD_HandleTypeDef *h) { note('I'); init_seen = h->Init; instance_seen = h->Instance; return fail_init ? HAL_ERROR : HAL_OK; }
HAL_StatusTypeDef HAL_HCD_Start(HCD_HandleTypeDef *h) { (void)h; note('S'); return fail_start ? HAL_ERROR : HAL_OK; }
HAL_StatusTypeDef HAL_HCD_Stop(HCD_HandleTypeDef *h) { (void)h; note('T'); return fail_stop ? HAL_ERROR : HAL_OK; }
HAL_StatusTypeDef HAL_HCD_DeInit(HCD_HandleTypeDef *h) { (void)h; note('X'); return fail_deinit ? HAL_ERROR : HAL_OK; }
void HAL_HCD_IRQHandler(HCD_HandleTypeDef *h) { irq_handled++; irq_handle = h; }

static void reset(void)
{
    memset(calls, 0, sizeof calls);
    call_count = gpio_count = 0u;
    clock_gpio = clock_usb_on = clock_usb_off = 0;
    fail_init = fail_start = fail_stop = fail_deinit = 0;
}

int main(void)
{
    UsbpConfig config = {8u, 1u, 0u, 5u};

    CHECK(UsbHostFs_Handle() == NULL && UsbHostFs_Close() == USBH_E_STATE, "a port that is not open has no handle and cannot be closed");

    /* bad configuration, nothing touched */
    reset();
    UsbpConfig bad = config;
    bad.host_channels = 13u;
    CHECK(UsbHostFs_Open(NULL) == USBH_E_ARG && UsbHostFs_Open(&bad) == USBH_E_ARG && gpio_count == 0u && call_count == 0u && clock_gpio == 0, "a configuration the plan refuses touches nothing");

    /* the start with the VBUS sense */
    reset();
    CHECK(UsbHostFs_Open(&config) == USBH_OK, "a good start");
    CHECK(strcmp(calls, "IPES") == 0, "order: initialise, priority, interrupt on, start");
    CHECK(gpio_count == 2u && gpio_port[0] == GPIOA && gpio_seen[0].Pin == (GPIO_PIN_11 | GPIO_PIN_12) && gpio_seen[0].Mode == GPIO_MODE_AF_PP && gpio_seen[0].Alternate == GPIO_AF10_OTG1_FS && gpio_seen[0].Speed == GPIO_SPEED_FREQ_VERY_HIGH && gpio_seen[0].Pull == GPIO_NOPULL, "PA11 and PA12 on alternate function 10");
    CHECK(gpio_port[1] == GPIOA && gpio_seen[1].Pin == GPIO_PIN_9 && gpio_seen[1].Mode == GPIO_MODE_INPUT && gpio_seen[1].Pull == GPIO_NOPULL, "PA9 is an input for the VBUS sense");
    CHECK(clock_gpio == 1 && clock_usb_on == 1 && instance_seen == USB_OTG_FS, "the clocks and the instance");
    CHECK(init_seen.Host_channels == 8u && init_seen.speed == HCD_SPEED_FULL && init_seen.dma_enable == DISABLE && init_seen.phy_itface == HCD_PHY_EMBEDDED && init_seen.low_power_enable == DISABLE && init_seen.use_external_vbus == DISABLE, "full speed, embedded PHY, no DMA, no low power");
    CHECK(init_seen.vbus_sensing_enable == ENABLE && init_seen.Sof_enable == DISABLE && priority_seen == 5u, "VBUS sense on, no SOF pin, priority 5");
    CHECK(UsbHostFs_Handle() != NULL && UsbHostFs_Open(&config) == USBH_E_STATE, "the handle exists and a second open is refused");
    OTG_FS_IRQHandler();
    CHECK(irq_handled == 1u && irq_handle == UsbHostFs_Handle(), "the interrupt hands the HAL its handle");

    /* the close */
    reset();
    CHECK(UsbHostFs_Close() == USBH_OK && strcmp(calls, "DTX") == 0 && clock_usb_off == 1 && UsbHostFs_Handle() == NULL, "close: interrupt off, stop, release, clock off");
    reset(); fail_stop = 1;
    CHECK(UsbHostFs_Open(&config) == USBH_OK, "open again");
    reset(); fail_stop = 1;
    CHECK(UsbHostFs_Close() == USBH_E_HAL && UsbHostFs_Handle() == NULL, "a stop that fails is reported and the port is closed anyway");
    reset();
    CHECK(UsbHostFs_Open(&config) == USBH_OK, "open for the next case");
    reset(); fail_deinit = 1;
    CHECK(UsbHostFs_Close() == USBH_E_HAL, "a release that fails is reported");

    /* no VBUS sense, the SOF pin on */
    reset();
    config.vbus_sensing = 0u; config.sof_output = 1u; config.host_channels = 12u;
    CHECK(UsbHostFs_Open(&config) == USBH_OK && gpio_count == 1u && init_seen.vbus_sensing_enable == DISABLE && init_seen.Sof_enable == ENABLE && init_seen.Host_channels == 12u, "without the VBUS sense PA9 is left alone");
    reset();
    CHECK(UsbHostFs_Close() == USBH_OK, "closed");

    /* refused HAL: everything is undone and the port stays closed */
    reset(); fail_init = 1;
    CHECK(UsbHostFs_Open(&config) == USBH_E_HAL && strcmp(calls, "I") == 0 && clock_usb_off == 1 && UsbHostFs_Handle() == NULL, "a refused initialisation: clock off, closed");
    reset(); fail_start = 1;
    CHECK(UsbHostFs_Open(&config) == USBH_E_HAL && strcmp(calls, "IPESDX") == 0 && clock_usb_off == 1 && UsbHostFs_Handle() == NULL, "a refused start: interrupt off, released, clock off");
    reset();
    CHECK(UsbHostFs_Open(&config) == USBH_OK, "and the port can be opened afterwards");

    if (failures) {
        printf("usb_host_fs host tests: %d FAILED\n", failures);
        return 1;
    }
    printf("usb_host_fs host logic tests passed\n");
    return 0;
}
