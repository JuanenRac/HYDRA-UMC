/* =============================================================================
 * HYDRA-UMC - src/mcu_stm32h757/CM7/UsbHostFs.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * The front USB port of the H757 board (USB_OTG_FS: D- on PA11, D+ on PA12, VBUS sense on PA9) as a full-speed host on the CM7: it starts the controller with the numbers of
 * Common/usb_host_plan.h (embedded PHY, no DMA, no low-power mode) and enables its interrupt. The class drivers that sit on top of the HAL host (storage, keyboard) are not part of
 * this unit; it hands them the HAL handle. OFF by default: with HYDRA_USB_HOST_CM7 0 (as it ships) UsbHostFs.c compiles to an empty unit and no pin or interrupt is touched. The power
 * switch of the port is not driven here (the line is not in the platform profile). Not run on a board: a full-speed device plugged in and enumerated is the check that remains.
 * ========================================================================== */
#ifndef HYDRA_UMC_USB_HOST_FS_H
#define HYDRA_UMC_USB_HOST_FS_H

#ifndef HYDRA_USB_HOST_CM7
#define HYDRA_USB_HOST_CM7 0
#endif

#if HYDRA_USB_HOST_CM7

#include <stdint.h>

#include "stm32h7xx_hal.h"
#include "usb_host_plan.h"

#define USBH_OK 0
#define USBH_E_ARG 1   /* the configuration was refused by the plan or is missing */
#define USBH_E_HAL 2   /* the HAL refused the initialisation or the start */
#define USBH_E_STATE 3 /* already open, or not open */

/* Starts the controller as a host. On a refusal everything the HAL had set up is undone and the port stays closed. */
int UsbHostFs_Open(const UsbpConfig *config);

/* Stops the host, switches the interrupt off and releases the controller. */
int UsbHostFs_Close(void);

/* The HAL handle for the class drivers, or NULL when the port is not open. */
HCD_HandleTypeDef *UsbHostFs_Handle(void);

#endif /* HYDRA_USB_HOST_CM7 */

#endif /* HYDRA_UMC_USB_HOST_FS_H */
