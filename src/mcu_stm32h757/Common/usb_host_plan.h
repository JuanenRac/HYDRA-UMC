/* =============================================================================
 * HYDRA-UMC Firmware - src/mcu_stm32h757/Common/usb_host_plan.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * usb_host_plan.h - the numbers that the front USB port of the H757 board (USB_OTG_FS, PA11 and PA12, VBUS sense on PA9) needs to run as a host (HAL-free, host-testable): how many
 * host channels are used (the controller has 12), whether the VBUS sense pin is used, whether the start-of-frame signal is put on a pin, and the priority of the interrupt. The three
 * FIFOs of the controller share 320 words; the low-level driver of the HAL fixes them at 128 words for reception, 96 for the non-periodic transmission and 64 for the periodic one
 * (288 words), and two static assertions hold that they fit the 320 words and that the receive FIFO can hold the largest packet and one status word for each of the 12 channels. The driver is CM7/UsbHostFs.c;
 * tools/usb_host_plan.py is the Python twin and reads config/usb_host.cfg. Nothing here has run on a board.
 * ============================================================================= */
#ifndef HYDRA_UMC_USB_HOST_PLAN_H
#define HYDRA_UMC_USB_HOST_PLAN_H

#include <stdint.h>

#define USBP_CHANNELS_MIN 1UL
#define USBP_CHANNELS_MAX 12UL
#define USBP_IRQ_PRIORITY_MAX 15UL

#define USBP_FIFO_TOTAL_WORDS 320UL
#define USBP_FIFO_RX_WORDS 128UL
#define USBP_FIFO_NPTX_WORDS 96UL
#define USBP_FIFO_PTX_WORDS 64UL
/* the receive FIFO holds, besides the largest packet of 64 bytes (16 words) and its status word, one status word for each channel in use */
#define USBP_RX_PACKET_WORDS 17UL

_Static_assert(USBP_FIFO_RX_WORDS + USBP_FIFO_NPTX_WORDS + USBP_FIFO_PTX_WORDS <= USBP_FIFO_TOTAL_WORDS, "the three FIFOs fit the 320 words of the controller");
_Static_assert(USBP_FIFO_RX_WORDS >= USBP_RX_PACKET_WORDS + USBP_CHANNELS_MAX, "the receive FIFO holds a packet and a status word for each channel");

#define USBP_OK 0
#define USBP_E_ARG 1
#define USBP_E_CHANNELS 2
#define USBP_E_FLAG 3 /* vbus_sensing or sof_output is not 0 or 1 */
#define USBP_E_PRIORITY 4

typedef struct {
    uint32_t host_channels;
    uint32_t vbus_sensing; /* 1 = PA9 senses VBUS */
    uint32_t sof_output;   /* 1 = the start-of-frame pulse is put on a pin */
    uint32_t irq_priority;
} UsbpConfig;

static inline int UsbPlan_Check(const UsbpConfig *c)
{
    if (!c) return USBP_E_ARG;
    if (c->host_channels < USBP_CHANNELS_MIN || c->host_channels > USBP_CHANNELS_MAX) return USBP_E_CHANNELS;
    if (c->vbus_sensing > 1u || c->sof_output > 1u) return USBP_E_FLAG;
    if (c->irq_priority > USBP_IRQ_PRIORITY_MAX) return USBP_E_PRIORITY;
    return USBP_OK;
}

/* The words of the three FIFOs that the HAL sets up; never more than the controller has. */
static inline uint32_t UsbPlan_FifoWordsUsed(void)
{
    return USBP_FIFO_RX_WORDS + USBP_FIFO_NPTX_WORDS + USBP_FIFO_PTX_WORDS;
}

#endif /* HYDRA_UMC_USB_HOST_PLAN_H */
