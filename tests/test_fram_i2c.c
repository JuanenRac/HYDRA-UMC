/* =============================================================================
 * HYDRA-UMC Firmware - host test of fram_i2c.h (the FM24CL64B driver on the local bus I2C4)
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * The bus and the chip are a fake (an array behind the same transfer function the HAL glue implements); nothing here has run on a board.
 */
#include <stdio.h>
#include <string.h>
#include "fram_i2c.h"

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s (line %d)\n", (msg), __LINE__); failures++; } } while (0)

typedef struct {
    uint8_t cells[FRAM_I2C_SIZE];
    unsigned transfers;
    unsigned yields;
    unsigned bad_address_byte;   /* transfers whose device address was not 0x50 */
    unsigned max_write_data;     /* the longest data part of one write */
    unsigned fail_next;          /* the next N transfers NACK */
    int nack_always;
    int corrupt_at;              /* a write that covers this address stores the wrong value (-1 never) */
    int corrupt_once;            /* corrupt only the first time */
} FakeChip;

static int fake_transfer(void *context, uint8_t address7, const uint8_t *tx, uint32_t tx_length, uint8_t *rx, uint32_t rx_length, uint32_t timeout_ms)
{
    FakeChip *c = (FakeChip *)context;
    uint32_t address;
    (void)timeout_ms;
    c->transfers++;
    if (address7 != FRAM_I2C_ADDRESS) c->bad_address_byte++;
    if (c->nack_always) return 1;
    if (c->fail_next > 0) { c->fail_next--; return 1; }
    if (tx_length < 2u) return 1;
    address = ((uint32_t)tx[0] << 8) | tx[1];
    if (address >= FRAM_I2C_SIZE) return 1;
    if (rx_length > 0) {
        for (uint32_t i = 0; i < rx_length; i++) rx[i] = c->cells[(address + i) % FRAM_I2C_SIZE]; /* an F-RAM wraps at the end */
        return 0;
    }
    if (tx_length - 2u > c->max_write_data) c->max_write_data = tx_length - 2u;
    for (uint32_t i = 0; i < tx_length - 2u; i++) {
        const uint32_t where = (address + i) % FRAM_I2C_SIZE;
        uint8_t value = tx[2u + i];
        if (c->corrupt_at >= 0 && (int)where == c->corrupt_at) {
            value = (uint8_t)(value ^ 0x01u);
            if (c->corrupt_once) c->corrupt_at = -1;
        }
        c->cells[where] = value;
    }
    return 0;
}

static void fake_yield(void *context)
{
    ((FakeChip *)context)->yields++;
}

static void reset(FakeChip *c)
{
    for (uint32_t i = 0; i < FRAM_I2C_SIZE; i++) c->cells[i] = (uint8_t)(0x30u + i * 7u);
    c->transfers = c->yields = c->bad_address_byte = c->max_write_data = c->fail_next = 0;
    c->nack_always = 0;
    c->corrupt_at = -1;
    c->corrupt_once = 0;
}

static FramI2c_t make(FakeChip *c, FramI2cBus_t *bus, int with_yield)
{
    FramI2c_t f;
    bus->transfer = fake_transfer;
    bus->yield = with_yield ? fake_yield : NULL;
    bus->context = c;
    FramI2c_Init(&f, bus);
    return f;
}

static void test_constants(void)
{
    CHECK(FRAM_I2C_ADDRESS == 0x50u, "A2..A0 to ground: the 7-bit address is 0x50");
    CHECK(FRAM_I2C_SIZE == 8192u, "the FM24CL64B is 8 KiB");
    CHECK(2u + FRAM_I2C_CHUNK <= 32u, "a chunk and its two address bytes fit in a buffer of 32");
}

static void test_read_and_two_byte_addressing(void)
{
    static FakeChip chip; FramI2cBus_t bus; FramI2c_t f; uint8_t out[100];
    reset(&chip);
    f = make(&chip, &bus, 1);
    CHECK(FramI2c_Read(&f, 0x0123u, out, 100) == 0, "a read of 100 bytes");
    CHECK(memcmp(out, &chip.cells[0x0123], 100) == 0, "it returns what the chip holds at that address (the high byte of the address is sent first)");
    CHECK(chip.bad_address_byte == 0, "every transfer goes to 0x50");
    CHECK(chip.transfers == (100u + FRAM_I2C_CHUNK - 1u) / FRAM_I2C_CHUNK, "a read is cut into chunks");
    CHECK(chip.yields == chip.transfers - 1u, "the bus is yielded between chunks, not after the last");
    CHECK(FramI2c_Read(&f, 0x1FF0u, out, 16) == 0 && memcmp(out, &chip.cells[0x1FF0], 16) == 0, "a read that ends exactly at the end of the chip");
}

static void test_refusals(void)
{
    static FakeChip chip; FramI2cBus_t bus; FramI2c_t f; uint8_t buf[8] = {0};
    reset(&chip);
    f = make(&chip, &bus, 0);
    CHECK(FramI2c_Read(&f, FRAM_I2C_SIZE - 4u, buf, 8) != 0, "a read past the end is refused, not wrapped");
    CHECK(FramI2c_Write(&f, FRAM_I2C_SIZE - 4u, buf, 8) != 0, "a write past the end is refused, not wrapped");
    CHECK(FramI2c_Read(&f, FRAM_I2C_SIZE + 1u, buf, 0) != 0, "an address past the end is refused");
    CHECK(chip.transfers == 0, "a refused access never touches the bus");
    CHECK(FramI2c_Read(&f, 0, NULL, 4) != 0 && FramI2c_Write(&f, 0, NULL, 4) != 0, "no buffer is a failure");
    {
        FramI2c_t bare; FramI2cBus_t empty;
        empty.transfer = NULL; empty.yield = NULL; empty.context = NULL;
        FramI2c_Init(&bare, &empty);
        CHECK(FramI2c_Read(&bare, 0, buf, 1) != 0 && FramI2c_Write(&bare, 0, buf, 1) != 0, "a bus without a transfer function is a failure");
        CHECK(FramI2c_Read(NULL, 0, buf, 1) != 0, "no driver is a failure");
    }
}

static void test_write_has_no_page_limit(void)
{
    static FakeChip chip; FramI2cBus_t bus; FramI2c_t f; uint8_t data[1000];
    reset(&chip);
    f = make(&chip, &bus, 1);
    for (unsigned i = 0; i < sizeof data; i++) data[i] = (uint8_t)(i * 13u + 5u);
    CHECK(FramI2c_Write(&f, 0x0FF0u, data, sizeof data) == 0, "a write of 1000 bytes that crosses 4 KiB and every page boundary of a flash");
    CHECK(memcmp(&chip.cells[0x0FF0], data, sizeof data) == 0, "the chip holds it");
    CHECK(chip.max_write_data <= FRAM_I2C_CHUNK, "no write carries more than a chunk");
    CHECK(chip.yields > 0, "the bus yields during a long write");
    CHECK(FramI2c_Write(&f, 0, data, 0) == 0, "an empty write is nothing");
    CHECK(FramI2c_Write(&f, FRAM_I2C_SIZE - 3u, data, 3) == 0 && memcmp(&chip.cells[FRAM_I2C_SIZE - 3u], data, 3) == 0, "the last bytes of the chip");
}

static void test_retries_and_timeouts(void)
{
    static FakeChip chip; FramI2cBus_t bus; FramI2c_t f; uint8_t out[10]; uint8_t data[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    reset(&chip);
    f = make(&chip, &bus, 0);
    chip.fail_next = FRAM_I2C_RETRIES - 1u;
    CHECK(FramI2c_Read(&f, 0, out, 10) == 0 && f.retries_used == FRAM_I2C_RETRIES - 1u, "a read that NACKs twice succeeds on the last try");
    chip.fail_next = FRAM_I2C_RETRIES;
    CHECK(FramI2c_Read(&f, 0, out, 10) != 0, "a read that NACKs every try fails");
    chip.fail_next = 1;
    CHECK(FramI2c_Write(&f, 100, data, 10) == 0 && memcmp(&chip.cells[100], data, 10) == 0, "a write that NACKs once is repeated");
    chip.nack_always = 1;
    chip.transfers = 0;
    CHECK(FramI2c_Write(&f, 100, data, 10) != 0, "a chip that never answers fails the write");
    CHECK(chip.transfers == FRAM_I2C_RETRIES, "and only the allowed number of tries is made");
}

static void test_write_is_verified(void)
{
    static FakeChip chip; FramI2cBus_t bus; FramI2c_t f; uint8_t data[40];
    reset(&chip);
    f = make(&chip, &bus, 0);
    for (unsigned i = 0; i < sizeof data; i++) data[i] = (uint8_t)(0xA0u + i);
    chip.corrupt_at = 5;
    CHECK(FramI2c_Write(&f, 0, data, sizeof data) != 0, "a cell that keeps the wrong value fails the write (the read-back catches it)");
    reset(&chip);
    chip.corrupt_at = 5;
    chip.corrupt_once = 1;
    CHECK(FramI2c_Write(&f, 0, data, sizeof data) == 0 && memcmp(chip.cells, data, sizeof data) == 0, "a glitch that happens once is repaired by the repeat of the chunk");
    CHECK(f.retries_used >= 1u, "and it is counted");
}

static void test_through_the_node(void)
{
    static FakeChip chip; FramI2cBus_t bus; FramI2c_t f; FramDriver_t d; FramNode_t n; uint8_t req[8], out[8];
    reset(&chip);
    f = make(&chip, &bus, 1);
    FramI2c_MakeDriver(&d, &f);
    CHECK(d.size == FRAM_I2C_SIZE && d.read != NULL && d.write != NULL, "the driver is the whole chip");
    FramNode_Init(&n, &d);
    n.enabled = 1;
    Fram_BuildRequest(req, FRAM_OP_ERASE, FRAM_BRAIN_ID);
    CHECK(FramNode_Handle(&n, FRAM_BRAIN_ID, 0, req, 8, out) == 1 && out[2] == FRAM_STATUS_OK, "an erase through the I2C driver is OK");
    CHECK(out[4] == 0 && out[5] == 0 && out[6] == 0x20 && out[7] == 0, "and says it wrote 8192 bytes");
    {
        int all_zero = 1;
        for (uint32_t i = 0; i < FRAM_I2C_SIZE; i++) if (chip.cells[i] != 0) all_zero = 0;
        CHECK(all_zero, "the whole chip is zero");
    }
    for (uint32_t i = 0; i < FRAM_I2C_SIZE; i++) chip.cells[i] = (uint8_t)(i ^ 0x5Au);
    Fram_BuildRequest(req, FRAM_OP_SELFTEST, FRAM_BRAIN_ID);
    CHECK(FramNode_Handle(&n, FRAM_BRAIN_ID, 0, req, 8, out) == 1 && out[2] == FRAM_STATUS_OK, "the self-test through the I2C driver is OK");
    {
        int kept = 1;
        for (uint32_t i = 0; i < FRAM_I2C_SIZE; i++) if (chip.cells[i] != (uint8_t)(i ^ 0x5Au)) kept = 0;
        CHECK(kept, "and puts back what was in the scratch area");
    }
    chip.nack_always = 1;
    Fram_BuildRequest(req, FRAM_OP_ERASE, FRAM_BRAIN_ID);
    CHECK(FramNode_Handle(&n, FRAM_BRAIN_ID, 0, req, 8, out) == 1 && out[2] == FRAM_STATUS_CHIP_ERROR, "a chip that does not answer is CHIP_ERROR, not a made-up success");
    chip.nack_always = 0;
    reset(&chip);
    chip.corrupt_at = 4000;
    Fram_BuildRequest(req, FRAM_OP_ERASE, FRAM_BRAIN_ID);
    CHECK(FramNode_Handle(&n, FRAM_BRAIN_ID, 0, req, 8, out) == 1 && out[2] == FRAM_STATUS_CHIP_ERROR, "a cell that keeps a wrong value stops the erase");
}

int main(void)
{
    test_constants();
    test_read_and_two_byte_addressing();
    test_refusals();
    test_write_has_no_page_limit();
    test_retries_and_timeouts();
    test_write_is_verified();
    test_through_the_node();
    if (failures) {
        printf("test_fram_i2c: %d FAILED\n", failures);
        return 1;
    }
    printf("test_fram_i2c: all passed\n");
    return 0;
}
