/* =============================================================================
 * HYDRA-UMC - src/mcu_stm32h757/CM7/UsbHostFs.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * USB_OTG_FS of the CM7 as a full-speed host; see UsbHostFs.h and Common/usb_host_plan.h. The pins are those of docs/platform_profiles.json (USB_OTG_FS).
 * ========================================================================== */
#include "UsbHostFs.h"

#if HYDRA_USB_HOST_CM7

static HCD_HandleTypeDef g_hcd;
static int g_open;

int UsbHostFs_Open(const UsbpConfig *config)
{
    if (UsbPlan_Check(config) != USBP_OK) return USBH_E_ARG;
    if (g_open) return USBH_E_STATE;
    __HAL_RCC_GPIOA_CLK_ENABLE();
    GPIO_InitTypeDef data = {0};
    data.Pin = GPIO_PIN_11 | GPIO_PIN_12;
    data.Mode = GPIO_MODE_AF_PP;
    data.Pull = GPIO_NOPULL;
    data.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    data.Alternate = GPIO_AF10_OTG1_FS;
    HAL_GPIO_Init(GPIOA, &data);
    if (config->vbus_sensing) {
        GPIO_InitTypeDef vbus = {0};
        vbus.Pin = GPIO_PIN_9;
        vbus.Mode = GPIO_MODE_INPUT;
        vbus.Pull = GPIO_NOPULL;
        HAL_GPIO_Init(GPIOA, &vbus);
    }
    __HAL_RCC_USB_OTG_FS_CLK_ENABLE();
    g_hcd.Instance = USB_OTG_FS;
    g_hcd.Init.Host_channels = config->host_channels;
    g_hcd.Init.speed = HCD_SPEED_FULL;
    g_hcd.Init.dma_enable = DISABLE;
    g_hcd.Init.phy_itface = HCD_PHY_EMBEDDED;
    g_hcd.Init.Sof_enable = config->sof_output ? ENABLE : DISABLE;
    g_hcd.Init.low_power_enable = DISABLE;
    g_hcd.Init.vbus_sensing_enable = config->vbus_sensing ? ENABLE : DISABLE;
    g_hcd.Init.use_external_vbus = DISABLE;
    if (HAL_HCD_Init(&g_hcd) != HAL_OK) {
        __HAL_RCC_USB_OTG_FS_CLK_DISABLE();
        return USBH_E_HAL;
    }
    HAL_NVIC_SetPriority(OTG_FS_IRQn, config->irq_priority, 0u);
    HAL_NVIC_EnableIRQ(OTG_FS_IRQn);
    if (HAL_HCD_Start(&g_hcd) != HAL_OK) {
        HAL_NVIC_DisableIRQ(OTG_FS_IRQn);
        (void)HAL_HCD_DeInit(&g_hcd);
        __HAL_RCC_USB_OTG_FS_CLK_DISABLE();
        return USBH_E_HAL;
    }
    g_open = 1;
    return USBH_OK;
}

int UsbHostFs_Close(void)
{
    if (!g_open) return USBH_E_STATE;
    HAL_NVIC_DisableIRQ(OTG_FS_IRQn);
    const HAL_StatusTypeDef stopped = HAL_HCD_Stop(&g_hcd);
    const HAL_StatusTypeDef released = HAL_HCD_DeInit(&g_hcd);
    __HAL_RCC_USB_OTG_FS_CLK_DISABLE();
    g_open = 0;
    return (stopped == HAL_OK && released == HAL_OK) ? USBH_OK : USBH_E_HAL;
}

HCD_HandleTypeDef *UsbHostFs_Handle(void)
{
    return g_open ? &g_hcd : NULL;
}

void OTG_FS_IRQHandler(void)
{
    HAL_HCD_IRQHandler(&g_hcd);
}

#endif /* HYDRA_USB_HOST_CM7 */
