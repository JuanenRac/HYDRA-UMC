/* =============================================================================
 * HYDRA-UMC Firmware - src/mcu_stm32h757/Common/tmc_card.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * tmc_card.h - the datagrams of the TMC5160 stepper drivers on the expansion cards and the choice of the card on SPI6 (HAL-free, host-testable). The drivers are on the cards,
 * not on the base board. SPI6 has one general chip select (PG3) for the three slots; the card is chosen by the two lines EXP_GPIO1 and EXP_GPIO2 (decision of the owner), which carry
 * the number of the slot (1 to 3; 0 is no card), and PG3 is the strobe of the transaction. Rules, all fail-safe:
 *   - a slot is selected only when the caller says it is READY (see expansion_slots.h); any other slot, 0 or a number above 3 is refused and nothing moves on the lines;
 *   - the select lines change only while PG3 is high, and PG3 goes low only after they have settled; at the end PG3 goes high first and the select lines return to 0;
 *   - a datagram is 5 bytes: the register address (bit 7 set for a write) and 32 bits of data, most significant first; the answer to it carries the status byte, and the data of
 *     a read arrives with the NEXT datagram, so a read is two datagrams (TmcCard_Read does both and returns the data of the second).
 * tests/test_tmc_card.c and tests/test_tmc_card.py hold the same golden datagrams. Nothing here has run on a board.
 * ============================================================================= */
#ifndef HYDRA_UMC_TMC_CARD_H
#define HYDRA_UMC_TMC_CARD_H

#include <stdint.h>

#define TMC_DATAGRAM_BYTES 5u
#define TMC_WRITE_BIT 0x80u
#define TMC_SLOTS 3u

/* bits of the status byte that comes back with every datagram */
#define TMC_STATUS_RESET_FLAG 0x01u
#define TMC_STATUS_DRIVER_ERROR 0x02u
#define TMC_STATUS_STALLGUARD 0x04u
#define TMC_STATUS_STANDSTILL 0x08u

typedef struct {
    void (*set_select)(void *ctx, uint8_t slot_bits);      /* EXP_GPIO1 is bit 0 and EXP_GPIO2 bit 1 */
    void (*set_cs)(void *ctx, uint8_t level);              /* PG3: 1 is idle (high), 0 is the transaction */
    int (*transfer)(void *ctx, const uint8_t *tx, uint8_t *rx, uint32_t length); /* 1 when the SPI6 peripheral worked */
    void (*settle)(void *ctx);                             /* waits the settling time of the select lines (configuration) */
} TmcCardOps;

typedef struct {
    uint8_t status;
    uint32_t data;
} TmcReply;

static inline void TmcCard_EncodeWrite(uint8_t reg, uint32_t value, uint8_t *out)
{
    out[0] = (uint8_t)(TMC_WRITE_BIT | (reg & 0x7Fu));
    out[1] = (uint8_t)(value >> 24);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 8);
    out[4] = (uint8_t)value;
}

static inline void TmcCard_EncodeRead(uint8_t reg, uint8_t *out)
{
    out[0] = (uint8_t)(reg & 0x7Fu);
    out[1] = 0;
    out[2] = 0;
    out[3] = 0;
    out[4] = 0;
}

static inline TmcReply TmcCard_DecodeReply(const uint8_t *in)
{
    TmcReply r;
    r.status = in[0];
    r.data = ((uint32_t)in[1] << 24) | ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 8) | (uint32_t)in[4];
    return r;
}

typedef enum { TMC_OK = 0, TMC_ERR_SLOT = 1, TMC_ERR_NOT_READY = 2, TMC_ERR_TRANSFER = 3 } TmcStatus;

/* One datagram to the card of `slot` (1 to 3). `ready_mask` has bit n-1 set for every slot the slot manager reports READY. */
static inline TmcStatus TmcCard_Exchange(const TmcCardOps *ops, void *ctx, uint8_t slot, uint8_t ready_mask, const uint8_t *tx, TmcReply *reply)
{
    uint8_t rx[TMC_DATAGRAM_BYTES];
    if (slot < 1u || slot > TMC_SLOTS) return TMC_ERR_SLOT;
    if (!((ready_mask >> (slot - 1u)) & 1u)) return TMC_ERR_NOT_READY;
    ops->set_cs(ctx, 1u);            /* the lines change only while the strobe is high */
    ops->set_select(ctx, slot);
    ops->settle(ctx);
    ops->set_cs(ctx, 0u);
    const int ok = ops->transfer(ctx, tx, rx, TMC_DATAGRAM_BYTES);
    ops->set_cs(ctx, 1u);            /* the strobe goes high first, then the card is let go */
    ops->set_select(ctx, 0u);
    if (!ok) return TMC_ERR_TRANSFER;
    *reply = TmcCard_DecodeReply(rx);
    return TMC_OK;
}

static inline TmcStatus TmcCard_Write(const TmcCardOps *ops, void *ctx, uint8_t slot, uint8_t ready_mask, uint8_t reg, uint32_t value, TmcReply *reply)
{
    uint8_t tx[TMC_DATAGRAM_BYTES];
    TmcCard_EncodeWrite(reg, value, tx);
    return TmcCard_Exchange(ops, ctx, slot, ready_mask, tx, reply);
}

/* A read is two datagrams: the first asks, the second (a read of the same register) carries the data. */
static inline TmcStatus TmcCard_Read(const TmcCardOps *ops, void *ctx, uint8_t slot, uint8_t ready_mask, uint8_t reg, TmcReply *reply)
{
    uint8_t tx[TMC_DATAGRAM_BYTES];
    TmcReply discard;
    TmcCard_EncodeRead(reg, tx);
    TmcStatus s = TmcCard_Exchange(ops, ctx, slot, ready_mask, tx, &discard);
    if (s != TMC_OK) return s;
    return TmcCard_Exchange(ops, ctx, slot, ready_mask, tx, reply);
}

#endif /* HYDRA_UMC_TMC_CARD_H */
