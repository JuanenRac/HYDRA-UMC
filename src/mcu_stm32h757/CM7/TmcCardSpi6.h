/* =============================================================================
 * HYDRA-UMC - src/mcu_stm32h757/CM7/TmcCardSpi6.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Driver of SPI6 as master towards the TMC5160 drivers of the expansion cards on the H757 board (SCK PG13, MISO PG12, MOSI PG14, general chip select PG3, card select lines
 * EXP_GPIO1 PE5 and EXP_GPIO2 PE6), under the datagrams and the card choice of Common/tmc_card.h. OFF by default: with HYDRA_TMC_CARD_SPI6 0 (as it ships) TmcCardSpi6.c compiles
 * to an empty unit and none of the pins is touched. The settling time and the transfer time-out are not constants: the caller passes them from config/tmc_card.cfg. A card is
 * addressed only when the caller says its slot is READY (Common/expansion_slots.h). Not run on a board: the logic analyser test of docs/EXPANSION_SLOTS.md remains.
 * ========================================================================== */
#ifndef HYDRA_UMC_TMC_CARD_SPI6_H
#define HYDRA_UMC_TMC_CARD_SPI6_H

#ifndef HYDRA_TMC_CARD_SPI6
#define HYDRA_TMC_CARD_SPI6 0
#endif

#if HYDRA_TMC_CARD_SPI6

#include <stdint.h>
#include "tmc_card.h"

typedef struct {
    uint32_t settle_us;                    /* how long the select lines settle before the strobe goes low (1..1000) */
    uint32_t transfer_timeout_ms;          /* how long one 5-byte datagram may take (1..1000) */
    void (*delay_us)(uint32_t microseconds); /* the busy wait of the application (required) */
} TmcCardSpi6Config;

/* Starts SPI6 and the lines (strobe high, no card selected). Returns 1 when the configuration and the peripheral were accepted; on 0 nothing can be sent. */
int TmcCardSpi6_Init(const TmcCardSpi6Config *cfg);

/* One register write or read on the card of `slot` (1 to 3); `ready_mask` has bit n-1 set for every slot the slot manager reports READY. Before a successful Init every call
 * answers TMC_ERR_TRANSFER and moves nothing. */
TmcStatus TmcCardSpi6_Write(uint8_t slot, uint8_t ready_mask, uint8_t reg, uint32_t value, TmcReply *reply);
TmcStatus TmcCardSpi6_Read(uint8_t slot, uint8_t ready_mask, uint8_t reg, TmcReply *reply);

#endif /* HYDRA_TMC_CARD_SPI6 */

#endif /* HYDRA_UMC_TMC_CARD_SPI6_H */
