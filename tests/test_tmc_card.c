/* =============================================================================
 * HYDRA-UMC Firmware - tests/test_tmc_card.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Host test of the datagrams of the TMC5160 cards and the choice of the card (src/mcu_stm32h757/Common/tmc_card.h) against a double of SPI6 and of the select lines: golden
 * datagrams, the order of the lines (the select lines change only while the strobe is high), the refusal of a slot that is not READY, and the two datagrams of a read.
 * Evidence: simulator.
 *
 *   cc -std=c11 -Wall -Wextra -Isrc/mcu_stm32h757/Common -o test_tmc_card tests/test_tmc_card.c
 */
#include <stdio.h>
#include <string.h>

#include "tmc_card.h"

typedef struct {
    uint8_t select, cs;
    int select_changed_with_cs_low;
    int transfers;
    uint8_t last_tx[TMC_DATAGRAM_BYTES];
    uint8_t select_during_transfer;
    uint8_t cs_during_transfer;
    int fail;
    uint8_t answer[2][TMC_DATAGRAM_BYTES];
    int answers_given;
    int order_ok;
    int settles;
} Double;

static void d_select(void *c, uint8_t bits)
{
    Double *d = (Double *)c;
    if (d->cs == 0u) d->select_changed_with_cs_low = 1;
    d->select = bits;
}
static void d_cs(void *c, uint8_t level) { ((Double *)c)->cs = level; }
static void d_settle(void *c) { ((Double *)c)->settles++; }
static int d_transfer(void *c, const uint8_t *tx, uint8_t *rx, uint32_t length)
{
    Double *d = (Double *)c;
    memcpy(d->last_tx, tx, length);
    d->select_during_transfer = d->select;
    d->cs_during_transfer = d->cs;
    d->transfers++;
    if (d->fail) return 0;
    memcpy(rx, d->answer[d->answers_given < 2 ? d->answers_given : 1], length);
    d->answers_given++;
    return 1;
}

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  FAIL: " __VA_ARGS__); printf("\n"); failures++; } } while (0)

int main(void)
{
    const TmcCardOps ops = {d_select, d_cs, d_transfer, d_settle};
    uint8_t tx[TMC_DATAGRAM_BYTES];

    /* golden datagrams */
    TmcCard_EncodeWrite(0x00u, 0x00000004UL, tx);
    CHECK(tx[0] == 0x80 && tx[1] == 0 && tx[2] == 0 && tx[3] == 0 && tx[4] == 0x04, "GCONF write");
    TmcCard_EncodeWrite(0x10u, 0x00071F0AUL, tx);
    CHECK(tx[0] == 0x90 && tx[1] == 0x00 && tx[2] == 0x07 && tx[3] == 0x1F && tx[4] == 0x0A, "IHOLD_IRUN write");
    TmcCard_EncodeWrite(0xFFu, 0xDEADBEEFUL, tx);
    CHECK(tx[0] == 0xFF && tx[1] == 0xDE && tx[4] == 0xEF, "the address is 7 bits and the write bit is set");
    TmcCard_EncodeRead(0x04u, tx);
    CHECK(tx[0] == 0x04 && tx[1] == 0 && tx[4] == 0, "IOIN read");
    TmcCard_EncodeRead(0xFFu, tx);
    CHECK(tx[0] == 0x7F, "a read never sets the write bit");
    {
        const uint8_t raw[5] = {0x09, 0x12, 0x34, 0x56, 0x78};
        const TmcReply r = TmcCard_DecodeReply(raw);
        CHECK(r.status == (TMC_STATUS_RESET_FLAG | TMC_STATUS_STANDSTILL) && r.data == 0x12345678UL, "reply decoding");
    }

    /* a write: strobe high, lines change, settle, strobe low during the transfer, strobe high, lines released */
    {
        Double d;
        TmcReply r;
        memset(&d, 0, sizeof d);
        d.cs = 1u;
        d.answer[0][0] = 0x01;
        CHECK(TmcCard_Write(&ops, &d, 2u, 0x07u, 0x00u, 4UL, &r) == TMC_OK, "write succeeds");
        CHECK(d.select_during_transfer == 2u && d.cs_during_transfer == 0u, "slot 2 selected and the strobe low during the transfer");
        CHECK(!d.select_changed_with_cs_low, "the select lines never changed while the strobe was low");
        CHECK(d.cs == 1u && d.select == 0u, "the strobe is high and no card is selected afterwards");
        CHECK(d.settles == 1 && r.status == 0x01, "settled once, status returned");
    }
    /* a read is two datagrams and the data of the second is the answer */
    {
        Double d;
        TmcReply r;
        memset(&d, 0, sizeof d);
        d.cs = 1u;
        d.answer[0][0] = 0x08;
        d.answer[1][0] = 0x08;
        d.answer[1][1] = 0xCA;
        d.answer[1][4] = 0xFE;
        CHECK(TmcCard_Read(&ops, &d, 3u, 0x04u, 0x04u, &r) == TMC_OK, "read succeeds");
        CHECK(d.transfers == 2 && r.data == 0xCA0000FEUL, "two datagrams, the data of the second");
        CHECK(!d.select_changed_with_cs_low, "no select change under the strobe in a read");
    }
    /* refusals: nothing moves on the lines */
    {
        Double d;
        TmcReply r;
        memset(&d, 0, sizeof d);
        d.cs = 1u;
        d.select = 0u;
        CHECK(TmcCard_Write(&ops, &d, 0u, 0x07u, 0, 0, &r) == TMC_ERR_SLOT, "slot 0 refused");
        CHECK(TmcCard_Write(&ops, &d, 4u, 0x07u, 0, 0, &r) == TMC_ERR_SLOT, "slot 4 refused");
        CHECK(TmcCard_Write(&ops, &d, 1u, 0x06u, 0, 0, &r) == TMC_ERR_NOT_READY, "a slot that is not READY refused");
        CHECK(TmcCard_Read(&ops, &d, 2u, 0x00u, 0, &r) == TMC_ERR_NOT_READY, "a read of a slot that is not READY refused");
        CHECK(d.transfers == 0 && d.settles == 0 && d.select == 0u && d.cs == 1u, "nothing moved on the lines");
    }
    /* a failing peripheral still releases the card, and a failing first datagram ends a read */
    {
        Double d;
        TmcReply r;
        memset(&d, 0, sizeof d);
        d.cs = 1u;
        d.fail = 1;
        CHECK(TmcCard_Write(&ops, &d, 1u, 0x01u, 0, 0, &r) == TMC_ERR_TRANSFER, "transfer failure reported");
        CHECK(d.cs == 1u && d.select == 0u, "the card is released after a failure");
        CHECK(TmcCard_Read(&ops, &d, 1u, 0x01u, 0, &r) == TMC_ERR_TRANSFER && d.transfers == 2, "a read stops after the first failing datagram");
    }
    if (failures) {
        printf("test_tmc_card: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_tmc_card: all checks passed\n");
    return 0;
}
