/* =============================================================================
 * HYDRA-UMC - src/mcu_stm32h757/CM7/NorSpi5.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Driver of SPI5 towards the NOR flash; see NorSpi5.h and Common/nor_flash.h. The kernel clock of SPI5 must be the one the profile lists (clocks_hz.kernel_spi5); selecting it is the
 * job of the clock set-up.
 * ========================================================================== */
#include "NorSpi5.h"

#if HYDRA_NOR_SPI5

#ifndef HYDRA_PLATFORM_H757
#error "NorSpi5 is the driver of the H757 board: compile it with HYDRA_PLATFORM_H757"
#endif
#include "platform_profile.h"

#define NOR_SPI5_PRESCALER 16UL
#if (HYDRA_CLOCK_KERNEL_SPI5_HZ / NOR_SPI5_PRESCALER) != HYDRA_NOR_SPI_CLOCK_HZ
#error "the prescaler of SPI5 does not give the clock of the profile"
#endif
#define NOR_SPI5_TIMEOUT_MAX 1000u

static SPI_HandleTypeDef hspi5_nor;
static NorSpi5Config g_cfg;
static int g_ready;

static void NorSpi5_Select(void *ctx, int asserted)
{
    (void)ctx;
    if (g_cfg.cs_port == NULL) return;
    HAL_GPIO_WritePin(g_cfg.cs_port, g_cfg.cs_pin, asserted ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

static int NorSpi5_Transfer(void *ctx, const uint8_t *tx, uint8_t *rx, uint32_t length)
{
    (void)ctx;
    HAL_StatusTypeDef status;
    if (!g_ready || length == 0u || length > 0xFFFFu) return 0;
    if (tx != NULL && rx != NULL) {
        status = HAL_SPI_TransmitReceive(&hspi5_nor, (uint8_t *)(uintptr_t)tx, rx, (uint16_t)length, g_cfg.transfer_timeout_ms);
    } else if (tx != NULL) {
        status = HAL_SPI_Transmit(&hspi5_nor, (uint8_t *)(uintptr_t)tx, (uint16_t)length, g_cfg.transfer_timeout_ms);
    } else if (rx != NULL) {
        status = HAL_SPI_Receive(&hspi5_nor, rx, (uint16_t)length, g_cfg.transfer_timeout_ms);
    } else {
        return 0;
    }
    if (status != HAL_OK) {
        (void)HAL_SPI_Abort(&hspi5_nor); /* a command that failed leaves nothing half-clocked for the next one */
        return 0;
    }
    return 1;
}

static const NorOps g_ops = {NorSpi5_Select, NorSpi5_Transfer};

const NorOps *NorSpi5_Ops(void)
{
    return &g_ops;
}

int NorSpi5_Init(const NorSpi5Config *cfg)
{
    g_ready = 0;
    if (cfg == NULL || cfg->cs_port == NULL || cfg->bus_port == NULL || cfg->bus_pins == 0u) return 0;
    if (cfg->cs_pin == 0u || (cfg->cs_pin & (uint16_t)(cfg->cs_pin - 1u)) != 0u) return 0;
    if (cfg->transfer_timeout_ms < 1u || cfg->transfer_timeout_ms > NOR_SPI5_TIMEOUT_MAX) return 0;
    g_cfg = *cfg;

    __HAL_RCC_SPI5_CLK_ENABLE();

    GPIO_InitTypeDef cs = {0};
    cs.Pin = g_cfg.cs_pin;
    cs.Mode = GPIO_MODE_OUTPUT_PP;
    cs.Pull = GPIO_PULLUP; /* the chip is not selected while the pin is not driven */
    cs.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(g_cfg.cs_port, &cs);
    NorSpi5_Select(NULL, 0);

    GPIO_InitTypeDef bus = {0};
    bus.Pin = g_cfg.bus_pins;
    bus.Mode = GPIO_MODE_AF_PP;
    bus.Pull = GPIO_NOPULL;
    bus.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    bus.Alternate = g_cfg.alternate;
    HAL_GPIO_Init(g_cfg.bus_port, &bus);

    hspi5_nor.Instance = SPI5;
    hspi5_nor.Init.Mode = SPI_MODE_MASTER;
    hspi5_nor.Init.Direction = SPI_DIRECTION_2LINES;
    hspi5_nor.Init.DataSize = SPI_DATASIZE_8BIT;
    hspi5_nor.Init.CLKPolarity = SPI_POLARITY_LOW;
    hspi5_nor.Init.CLKPhase = SPI_PHASE_1EDGE;
    hspi5_nor.Init.NSS = SPI_NSS_SOFT;
    hspi5_nor.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_16;
    hspi5_nor.Init.FirstBit = SPI_FIRSTBIT_MSB;
    hspi5_nor.Init.TIMode = SPI_TIMODE_DISABLE;
    hspi5_nor.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    if (HAL_SPI_Init(&hspi5_nor) != HAL_OK) return 0;
    g_ready = 1;
    return 1;
}

#endif /* HYDRA_NOR_SPI5 */
