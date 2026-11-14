/* =============================================================================
 * HYDRA-UMC - src/mcu_stm32h757/CM7/TmcCardSpi6.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Driver of SPI6 as master towards the TMC5160 cards; see TmcCardSpi6.h and Common/tmc_card.h. The pins and the clock come from the platform profile
 * (docs/platform_profiles.json: new_peripherals.SPI6 and expansion.lines). The kernel clock of SPI6 must be the one the profile lists (clocks_hz.kernel_spi6); selecting it is the
 * job of the clock set-up. The TMC5160 clocks in SPI mode 3 (clock idles high, data on the second edge).
 * ========================================================================== */
#include "TmcCardSpi6.h"

#if HYDRA_TMC_CARD_SPI6

#include "stm32h7xx_hal.h"

#ifndef HYDRA_PLATFORM_H757
#error "TmcCardSpi6 is the driver of the H757 board: compile it with HYDRA_PLATFORM_H757"
#endif
#include "platform_profile.h"

#define TMC_SPI6_PRESCALER 32UL
#define TMC_SPI6_CLOCK_HZ 3125000UL /* new_peripherals.SPI6.clock_hz of the profile */
#if (HYDRA_CLOCK_KERNEL_SPI6_HZ / TMC_SPI6_PRESCALER) != TMC_SPI6_CLOCK_HZ
#error "the prescaler of SPI6 does not give the clock of the profile"
#endif

#define TMC_SPI6_LIMIT_MAX 1000u

static SPI_HandleTypeDef hspi6_cards;
static TmcCardSpi6Config g_cfg;
static int g_ready;

static void TmcCardSpi6_SetSelect(void *ctx, uint8_t slot_bits)
{
    (void)ctx;
    HAL_GPIO_WritePin(GPIOE, GPIO_PIN_5, (slot_bits & 1u) ? GPIO_PIN_SET : GPIO_PIN_RESET); /* EXP_GPIO1 */
    HAL_GPIO_WritePin(GPIOE, GPIO_PIN_6, (slot_bits & 2u) ? GPIO_PIN_SET : GPIO_PIN_RESET); /* EXP_GPIO2 */
}

static void TmcCardSpi6_SetCs(void *ctx, uint8_t level)
{
    (void)ctx;
    HAL_GPIO_WritePin(GPIOG, GPIO_PIN_3, level ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static int TmcCardSpi6_Transfer(void *ctx, const uint8_t *tx, uint8_t *rx, uint32_t length)
{
    (void)ctx;
    const HAL_StatusTypeDef status = HAL_SPI_TransmitReceive(&hspi6_cards, (uint8_t *)(uintptr_t)tx, rx, (uint16_t)length, g_cfg.transfer_timeout_ms);
    if (status != HAL_OK) {
        (void)HAL_SPI_Abort(&hspi6_cards); /* a datagram that failed leaves nothing half-clocked for the next one */
        return 0;
    }
    return 1;
}

static void TmcCardSpi6_Settle(void *ctx)
{
    (void)ctx;
    g_cfg.delay_us(g_cfg.settle_us);
}

static const TmcCardOps g_ops = {TmcCardSpi6_SetSelect, TmcCardSpi6_SetCs, TmcCardSpi6_Transfer, TmcCardSpi6_Settle};

int TmcCardSpi6_Init(const TmcCardSpi6Config *cfg)
{
    g_ready = 0;
    if (cfg == NULL || cfg->delay_us == NULL) return 0;
    if (cfg->settle_us < 1u || cfg->settle_us > TMC_SPI6_LIMIT_MAX) return 0;
    if (cfg->transfer_timeout_ms < 1u || cfg->transfer_timeout_ms > TMC_SPI6_LIMIT_MAX) return 0;
    g_cfg = *cfg;

    __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_GPIOG_CLK_ENABLE();
    __HAL_RCC_SPI6_CLK_ENABLE();

    GPIO_InitTypeDef select = {0};
    select.Pin = GPIO_PIN_5 | GPIO_PIN_6; /* EXP_GPIO1 and EXP_GPIO2 */
    select.Mode = GPIO_MODE_OUTPUT_PP;
    select.Pull = GPIO_PULLDOWN; /* no card is selected while the pins are not driven */
    select.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOE, &select);
    TmcCardSpi6_SetSelect(NULL, 0u);

    GPIO_InitTypeDef cs = {0};
    cs.Pin = GPIO_PIN_3; /* the strobe of the transaction */
    cs.Mode = GPIO_MODE_OUTPUT_PP;
    cs.Pull = GPIO_PULLUP;
    cs.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(GPIOG, &cs);
    TmcCardSpi6_SetCs(NULL, 1u);

    GPIO_InitTypeDef bus = {0};
    bus.Pin = GPIO_PIN_12 | GPIO_PIN_13 | GPIO_PIN_14; /* MISO, SCK and MOSI: alternate function 5 */
    bus.Mode = GPIO_MODE_AF_PP;
    bus.Pull = GPIO_NOPULL;
    bus.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    bus.Alternate = GPIO_AF5_SPI6;
    HAL_GPIO_Init(GPIOG, &bus);

    hspi6_cards.Instance = SPI6;
    hspi6_cards.Init.Mode = SPI_MODE_MASTER;
    hspi6_cards.Init.Direction = SPI_DIRECTION_2LINES;
    hspi6_cards.Init.DataSize = SPI_DATASIZE_8BIT;
    hspi6_cards.Init.CLKPolarity = SPI_POLARITY_HIGH;
    hspi6_cards.Init.CLKPhase = SPI_PHASE_2EDGE;
    hspi6_cards.Init.NSS = SPI_NSS_SOFT;
    hspi6_cards.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_32;
    hspi6_cards.Init.FirstBit = SPI_FIRSTBIT_MSB;
    hspi6_cards.Init.TIMode = SPI_TIMODE_DISABLE;
    hspi6_cards.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    if (HAL_SPI_Init(&hspi6_cards) != HAL_OK) return 0;
    g_ready = 1;
    return 1;
}

TmcStatus TmcCardSpi6_Write(uint8_t slot, uint8_t ready_mask, uint8_t reg, uint32_t value, TmcReply *reply)
{
    if (!g_ready) return TMC_ERR_TRANSFER;
    return TmcCard_Write(&g_ops, NULL, slot, ready_mask, reg, value, reply);
}

TmcStatus TmcCardSpi6_Read(uint8_t slot, uint8_t ready_mask, uint8_t reg, TmcReply *reply)
{
    if (!g_ready) return TMC_ERR_TRANSFER;
    return TmcCard_Read(&g_ops, NULL, slot, ready_mask, reg, reply);
}

#endif /* HYDRA_TMC_CARD_SPI6 */
