/* =============================================================================
 * HYDRA-UMC - src/mcu_stm32h757/CM7/HostLinkSpi3.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Driver of SPI3 as master towards the Jetson; see HostLinkSpi3.h and Common/host_link_master.h. The pins and the clock come from the platform profile
 * (docs/platform_profiles.json: host_spi). The kernel clock of SPI3 must be the one the profile lists (clocks_hz.kernel_spi3); selecting it is the job of the clock set-up.
 * The READY line is not drawn in the schematic yet: while HYDRA_HOST_READY_PIN is not defined the line is not read and the transport must be configured with ready_known 0.
 * ========================================================================== */
#include "HostLinkSpi3.h"

#if HYDRA_HOST_LINK_MASTER

#include "stm32h7xx_hal.h"

#ifndef HYDRA_PLATFORM_H757
#error "HostLinkSpi3 is the driver of the H757 board: compile it with HYDRA_PLATFORM_H757"
#endif
#include "platform_profile.h"

#if HYDRA_HOST_SPI_INSTANCE != 3
#error "the host link of this platform is not SPI3"
#endif
#if (HYDRA_CLOCK_KERNEL_SPI3_HZ / HYDRA_HOST_SPI_PRESCALER) != HYDRA_HOST_SPI_CLOCK_HZ
#error "the prescaler of the host link does not give the clock of the profile"
#endif

#if HYDRA_HOST_SPI_PRESCALER == 2
#define HOST_LINK_BAUD SPI_BAUDRATEPRESCALER_2
#elif HYDRA_HOST_SPI_PRESCALER == 4
#define HOST_LINK_BAUD SPI_BAUDRATEPRESCALER_4
#elif HYDRA_HOST_SPI_PRESCALER == 8
#define HOST_LINK_BAUD SPI_BAUDRATEPRESCALER_8
#elif HYDRA_HOST_SPI_PRESCALER == 16
#define HOST_LINK_BAUD SPI_BAUDRATEPRESCALER_16
#elif HYDRA_HOST_SPI_PRESCALER == 32
#define HOST_LINK_BAUD SPI_BAUDRATEPRESCALER_32
#elif HYDRA_HOST_SPI_PRESCALER == 64
#define HOST_LINK_BAUD SPI_BAUDRATEPRESCALER_64
#elif HYDRA_HOST_SPI_PRESCALER == 128
#define HOST_LINK_BAUD SPI_BAUDRATEPRESCALER_128
#elif HYDRA_HOST_SPI_PRESCALER == 256
#define HOST_LINK_BAUD SPI_BAUDRATEPRESCALER_256
#else
#error "the prescaler of the host link is not one of the values of the peripheral"
#endif

#define HOST_LINK_TRANSFER_TIMEOUT_MS 20u

static SPI_HandleTypeDef hspi3_host;
static HostLink_t g_host_link;

static int HostLinkSpi3_Transfer(void *ctx, const uint8_t *tx, uint8_t *rx)
{
    (void)ctx;
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_15, GPIO_PIN_RESET);
    const HAL_StatusTypeDef status = HAL_SPI_TransmitReceive(&hspi3_host, (uint8_t *)(uintptr_t)tx, rx, HOST_LINK_FRAME_BYTES, HOST_LINK_TRANSFER_TIMEOUT_MS);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_15, GPIO_PIN_SET);
    if (status != HAL_OK) {
        (void)HAL_SPI_Abort(&hspi3_host); /* a transaction that failed leaves nothing half-clocked for the next one */
        return 0;
    }
    return 1;
}

static int HostLinkSpi3_Ready(void *ctx)
{
    (void)ctx;
#ifdef HYDRA_HOST_READY_PIN
    return HAL_GPIO_ReadPin(HYDRA_HOST_READY_PORT, HYDRA_HOST_READY_PIN) == GPIO_PIN_SET;
#else
    return 0; /* the line is not wired in the schematic: the transport runs on the idle interval alone */
#endif
}

int HostLinkSpi3_Init(const HostLinkConfig *cfg, int (*dispatch)(void *ctx, const uint8_t *request, uint8_t *answer12), void *ctx)
{
    HostLinkOps ops = {HostLinkSpi3_Transfer, HostLinkSpi3_Ready, dispatch};
#ifndef HYDRA_HOST_READY_PIN
    if (cfg->ready_known) return 0; /* the configuration says the line is wired but this build has no pin for it */
#endif
    if (!HostLink_Init(&g_host_link, cfg, &ops, ctx)) return 0;

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_SPI3_CLK_ENABLE();

    GPIO_InitTypeDef nss = {0};
    nss.Pin = GPIO_PIN_15;
    nss.Mode = GPIO_MODE_OUTPUT_PP;
    nss.Pull = GPIO_PULLUP;
    nss.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(GPIOA, &nss);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_15, GPIO_PIN_SET);

    GPIO_InitTypeDef clk_and_miso = {0};
    clk_and_miso.Pin = GPIO_PIN_3 | GPIO_PIN_4; /* SCK and MISO: alternate function 6 */
    clk_and_miso.Mode = GPIO_MODE_AF_PP;
    clk_and_miso.Pull = GPIO_NOPULL;
    clk_and_miso.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    clk_and_miso.Alternate = GPIO_AF6_SPI3;
    HAL_GPIO_Init(GPIOB, &clk_and_miso);

    GPIO_InitTypeDef mosi = {0};
    mosi.Pin = GPIO_PIN_5; /* MOSI: alternate function 7 */
    mosi.Mode = GPIO_MODE_AF_PP;
    mosi.Pull = GPIO_NOPULL;
    mosi.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    mosi.Alternate = GPIO_AF7_SPI3;
    HAL_GPIO_Init(GPIOB, &mosi);

    hspi3_host.Instance = SPI3;
    hspi3_host.Init.Mode = SPI_MODE_MASTER;
    hspi3_host.Init.Direction = SPI_DIRECTION_2LINES;
    hspi3_host.Init.DataSize = SPI_DATASIZE_8BIT;
    hspi3_host.Init.CLKPolarity = SPI_POLARITY_LOW;
    hspi3_host.Init.CLKPhase = SPI_PHASE_1EDGE;
    hspi3_host.Init.NSS = SPI_NSS_SOFT;
    hspi3_host.Init.BaudRatePrescaler = HOST_LINK_BAUD;
    hspi3_host.Init.FirstBit = SPI_FIRSTBIT_MSB;
    hspi3_host.Init.TIMode = SPI_TIMODE_DISABLE;
    hspi3_host.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    return HAL_SPI_Init(&hspi3_host) == HAL_OK ? 1 : 0;
}

HostLinkPoll HostLinkSpi3_Poll(uint32_t now_ms)
{
    return HostLink_Poll(&g_host_link, now_ms);
}

HostLink_t *HostLinkSpi3_Instance(void)
{
    return &g_host_link;
}

#endif /* HYDRA_HOST_LINK_MASTER */
