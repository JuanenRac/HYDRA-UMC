/* =============================================================================
 * HYDRA-UMC Firmware - Host test stand-in for the STM32H7 HAL header (GPIO, interrupt and USB host)
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Just enough of the vendor header for the USB host of the CM7 (src/mcu_stm32h757/CM7/UsbHostFs.c) to compile with the host's plain gcc. The names and the fields are the ones of the
 * HAL of the H7; the functions are declared here and defined by the test, which plays the pins, the interrupt controller and the USB controller and records what the driver did.
 */
#ifndef HYDRA_UMC_HOST_STUB_H7_USB_HAL_H
#define HYDRA_UMC_HOST_STUB_H7_USB_HAL_H

#include <stddef.h>
#include <stdint.h>

typedef enum { HAL_OK = 0, HAL_ERROR = 1, HAL_BUSY = 2, HAL_TIMEOUT = 3 } HAL_StatusTypeDef;

#define DISABLE 0U
#define ENABLE 1U

typedef struct { int port_id; } GPIO_TypeDef;
typedef struct { int instance_id; } USB_OTG_GlobalTypeDef;

typedef struct {
    uint32_t Pin, Mode, Pull, Speed, Alternate;
} GPIO_InitTypeDef;

typedef struct {
    uint32_t Host_channels, speed, dma_enable, phy_itface, Sof_enable, low_power_enable, vbus_sensing_enable, use_external_vbus;
} HCD_InitTypeDef;

typedef struct {
    USB_OTG_GlobalTypeDef *Instance;
    HCD_InitTypeDef Init;
} HCD_HandleTypeDef;

typedef enum { OTG_FS_IRQn = 67 } IRQn_Type;

extern GPIO_TypeDef stub_gpioa;
extern USB_OTG_GlobalTypeDef stub_otg_fs;
#define GPIOA (&stub_gpioa)
#define USB_OTG_FS (&stub_otg_fs)

#define GPIO_PIN_9 (1u << 9)
#define GPIO_PIN_11 (1u << 11)
#define GPIO_PIN_12 (1u << 12)
#define GPIO_MODE_INPUT 0u
#define GPIO_MODE_AF_PP 2u
#define GPIO_NOPULL 0u
#define GPIO_SPEED_FREQ_VERY_HIGH 3u
#define GPIO_AF10_OTG1_FS 10u

#define HCD_SPEED_FULL 1U
#define HCD_PHY_EMBEDDED 1U

void stub_clock(int which);
#define __HAL_RCC_GPIOA_CLK_ENABLE() stub_clock(1)
#define __HAL_RCC_USB_OTG_FS_CLK_ENABLE() stub_clock(2)
#define __HAL_RCC_USB_OTG_FS_CLK_DISABLE() stub_clock(-2)

void HAL_GPIO_Init(GPIO_TypeDef *port, GPIO_InitTypeDef *init);
void HAL_NVIC_SetPriority(IRQn_Type irq, uint32_t preempt, uint32_t sub);
void HAL_NVIC_EnableIRQ(IRQn_Type irq);
void HAL_NVIC_DisableIRQ(IRQn_Type irq);
HAL_StatusTypeDef HAL_HCD_Init(HCD_HandleTypeDef *hhcd);
HAL_StatusTypeDef HAL_HCD_Start(HCD_HandleTypeDef *hhcd);
HAL_StatusTypeDef HAL_HCD_Stop(HCD_HandleTypeDef *hhcd);
HAL_StatusTypeDef HAL_HCD_DeInit(HCD_HandleTypeDef *hhcd);
void HAL_HCD_IRQHandler(HCD_HandleTypeDef *hhcd);

#endif /* HYDRA_UMC_HOST_STUB_H7_USB_HAL_H */
