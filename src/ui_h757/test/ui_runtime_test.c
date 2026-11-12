/* =============================================================================
 * HYDRA-UMC Firmware - src/ui_h757/test/ui_runtime_test.c
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * Host test of the runtime of the interface (src/ui_h757/core/ui_runtime.c) against fakes: a Jetson double on the SPI link (answers the status, or stays silent), the F-RAM driver of
 * the brain (src/mcu_stm32h757/CM4/fram_i2c.h) over a fake I2C bus, and ports that record what the panel asks the machine to do. It checks the whole chain end to end: the Jetson
 * hangs and comes back, STOP goes out first and without touching the link, the operator PIN locks and unlocks, the failures counted survive a restart, and the F-RAM erase of the
 * brain (zeros over the chip) leaves no PIN accepted. Evidence: simulator.
 *
 *   cc -std=c11 -Wall -Wextra -Isrc/ui_h757/core -Isrc/mcu_stm32h757/CM4 -o ui_runtime_test src/ui_h757/test/ui_runtime_test.c src/ui_h757/core/ui_runtime.c \
 *      src/ui_h757/core/ui_core.c src/ui_h757/core/ui_link.c src/ui_h757/core/ui_pin.c
 */
#include <stdio.h>
#include <string.h>

#include "ui_pin_fram.h"
#include "ui_runtime.h"

static int failures;

#define CHECK(condition, message)                                   \
    do {                                                            \
        if (!(condition)) {                                         \
            printf("  FAIL: %s (line %d)\n", (message), __LINE__);  \
            failures++;                                             \
        }                                                           \
    } while (0)

/* ------------------------------------------------------------------ the fake I2C bus with the F-RAM */
typedef struct {
    uint8_t chip[FRAM_I2C_SIZE];
} fake_chip_t;

static int chip_transfer(void *context, uint8_t address7, const uint8_t *tx, uint32_t tx_length, uint8_t *rx, uint32_t rx_length, uint32_t timeout_ms)
{
    fake_chip_t *c = (fake_chip_t *)context;
    uint32_t address;
    (void)timeout_ms;
    if (address7 != FRAM_I2C_ADDRESS || tx_length < 2u) {
        return 1;
    }
    address = ((uint32_t)tx[0] << 8) | tx[1];
    if (rx_length > 0u) {
        if (address + rx_length > FRAM_I2C_SIZE) {
            return 1;
        }
        memcpy(rx, c->chip + address, rx_length);
        return 0;
    }
    if (address + (tx_length - 2u) > FRAM_I2C_SIZE) {
        return 1;
    }
    memcpy(c->chip + address, tx + 2, tx_length - 2u);
    return 0;
}

/* ------------------------------------------------------------------ the Jetson double */
typedef struct {
    int silent;
    int zone;
    uint32_t requests;
} jetson_t;

static int32_t jetson_transact(void *context, const uint8_t *request, uint32_t request_length, uint8_t *answer, uint32_t capacity, uint64_t now_ms)
{
    jetson_t *j = (jetson_t *)context;
    uint16_t sequence = (uint16_t)(request[2] | (request[3] << 8));
    uint8_t payload[10];
    (void)request_length;
    (void)capacity;
    (void)now_ms;
    j->requests++;
    if (j->silent) {
        return -1;
    }
    if (request[1] == UI_LINK_T_STATUS_REQ) {
        memset(payload, 0, sizeof(payload));
        payload[0] = 1; /* version */
        payload[1] = (uint8_t)j->zone;
        payload[2] = 23;
        return (int32_t)ui_link_encode(UI_LINK_T_STATUS, sequence, payload, sizeof(payload), answer);
    }
    payload[0] = 1; /* no newer picture */
    return (int32_t)ui_link_encode(UI_LINK_T_SNAPSHOT_NONE, sequence, payload, 1, answer);
}

/* ------------------------------------------------------------------ the ports that record what the panel asks */
typedef struct {
    uint32_t stops;
    uint32_t releases;
    uint32_t jogs;
    uint8_t last_jog;
    uint32_t settings;
    uint32_t spi_requests_at_stop;
    jetson_t *jetson;
} machine_t;

static void m_stop(void *context, uint8_t reason)
{
    machine_t *m = (machine_t *)context;
    m->stops++;
    m->spi_requests_at_stop = m->jetson->requests;
    (void)reason;
}

static void m_release(void *context)
{
    ((machine_t *)context)->releases++;
}

static void m_jog(void *context, uint8_t action)
{
    machine_t *m = (machine_t *)context;
    m->jogs++;
    m->last_jog = action;
}

static void m_setting(void *context, uint8_t key, uint8_t value)
{
    (void)key;
    (void)value;
    ((machine_t *)context)->settings++;
}

typedef struct {
    fake_chip_t chip;
    FramI2cBus_t bus;
    FramI2c_t fram;
    ui_pin_fram_t adapter;
    ui_pin_store_t store;
    jetson_t jetson;
    machine_t machine;
    ui_runtime_t rt;
    uint8_t picture[2048];
} rig_t;

static const uint8_t k_salt[16] = {9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 1, 2, 3, 4, 5, 6};

static ui_config_t config(int pin)
{
    ui_config_t cfg = {1500u, 90000u, 5u, 500u, 1000u, 1500u, 1500u, 10000u, 96u, 0u, 0u, 0u, 3u, 30000u, 120000u};
    cfg.operator_pin_enabled = (uint32_t)pin;
    return cfg;
}

static void open_store(rig_t *r)
{
    ui_pin_ports_t ports;
    r->bus.transfer = chip_transfer;
    r->bus.yield = 0;
    r->bus.context = &r->chip;
    FramI2c_Init(&r->fram, &r->bus);
    r->adapter.fram = &r->fram;
    ui_pin_fram_ports(&ports, &r->adapter);
    ui_pin_init(&r->store, &ports, 50, 4, 8); /* few iterations: the test is about the chain, the hash is tested elsewhere */
}

static void start(rig_t *r, int pin)
{
    ui_runtime_ports_t ports;
    ui_config_t cfg = config(pin);
    ports.spi_transact = jetson_transact;
    ports.spi_context = &r->jetson;
    ports.send_stop = m_stop;
    ports.send_release = m_release;
    ports.send_jog = m_jog;
    ports.save_setting = m_setting;
    ports.context = &r->machine;
    r->machine.jetson = &r->jetson;
    ui_runtime_init(&r->rt, &cfg, &ports, &r->store, r->picture, sizeof(r->picture));
}

static void run(rig_t *r, uint64_t from, uint64_t to)
{
    for (uint64_t t = from; t <= to; t += 50) {
        ui_runtime_poll(&r->rt, t);
    }
}

static void type(rig_t *r, uint64_t now, const char *digits)
{
    for (const char *p = digits; *p != '\0'; p++) {
        ui_runtime_pad_digit(&r->rt, *p - '0');
    }
    ui_runtime_pad_enter(&r->rt, now);
}

static void hang_and_recover(void)
{
    static rig_t r;
    memset(&r, 0, sizeof(r));
    open_store(&r);
    start(&r, 0);
    r.jetson.zone = 0;
    run(&r, 0, 1000);
    CHECK(r.rt.core.mode == UI_MODE_NORMAL && r.rt.core.link == UI_LINK_OK, "a Jetson that answers: normal");
    ui_runtime_touch(&r.rt, 1050, UI_ACT_JOG_ARM, 1);
    ui_runtime_touch(&r.rt, 1100, UI_ACT_JOG_X_PLUS, 1);
    CHECK(r.machine.jogs == 1 && r.machine.last_jog == UI_ACT_JOG_X_PLUS, "the jog reaches the machine while the Jetson answers");
    r.jetson.silent = 1;
    run(&r, 1150, 3500);
    CHECK(r.rt.core.mode == UI_MODE_DEGRADED, "the Jetson hangs: degraded");
    ui_runtime_touch(&r.rt, 3600, UI_ACT_JOG_ARM, 1);
    ui_runtime_touch(&r.rt, 3650, UI_ACT_JOG_X_PLUS, 1);
    CHECK(r.machine.jogs == 1, "nothing moves with the Jetson silent");
    {
        uint32_t asked = r.jetson.requests;
        ui_runtime_touch(&r.rt, 3700, UI_ACT_STOP, 1);
        CHECK(r.machine.stops == 1 && r.machine.spi_requests_at_stop == asked, "STOP goes out at once and without a single request to the Jetson");
        CHECK(r.jetson.requests == asked, "the stop path does not touch the link");
    }
    CHECK(r.rt.core.mode == UI_MODE_STOPPED, "stopped");
    ui_runtime_estop(&r.rt, 3750, 1);
    r.jetson.silent = 0;
    run(&r, 3800, 4200);
    CHECK(r.rt.core.link == UI_LINK_OK, "the Jetson is back");
    ui_runtime_touch(&r.rt, 4300, UI_ACT_RELEASE, 1);
    run(&r, 4350, 5900);
    CHECK(r.machine.releases == 1, "the release goes out after the long press, with the Jetson back");
    ui_runtime_touch(&r.rt, 6000, UI_ACT_NEXT_LANGUAGE, 1);
    CHECK(r.machine.settings == 1, "a setting is saved");
}

static void stop_is_always_first(void)
{
    static rig_t r;
    memset(&r, 0, sizeof(r));
    open_store(&r);
    start(&r, 1); /* PIN on and locked, link bus off, listen-only: STOP still goes out */
    run(&r, 0, 200);
    ui_runtime_bus(&r.rt, 250, 1, 255, 255);
    ui_runtime_touch(&r.rt, 300, UI_ACT_TOGGLE_LISTEN_ONLY, 1);
    CHECK(r.rt.core.listen_only, "listen-only turns on without the PIN");
    for (int i = 0; i < 5; i++) {
        ui_runtime_touch(&r.rt, 400u + (uint64_t)i * 10u, UI_ACT_STOP, 1);
    }
    CHECK(r.machine.stops == 5, "every touch of STOP is sent: locked, bus off, listen-only");
    CHECK(r.rt.stops_sent == 5, "and counted");
}

static void pin_chain(void)
{
    static rig_t r;
    memset(&r, 0, sizeof(r));
    open_store(&r);
    CHECK(ui_pin_set(&r.store, "4821", k_salt) == UI_PIN_SET_OK, "the PIN is set (from the Studio)");
    CHECK(UI_PIN_FRAM_BASE + UI_PIN_SLOTS * UI_PIN_RECORD_BYTES <= FRAM_I2C_SIZE - FRAM_SELFTEST_BYTES, "the slots do not touch the scratch area of the self-test");
    start(&r, 1);
    run(&r, 0, 500);
    ui_runtime_touch(&r.rt, 550, UI_ACT_JOG_ARM, 1);
    CHECK(r.rt.core.pin_unlocked == 0 && r.rt.core.jog_armed_until == 0, "locked at start: the jog is refused");
    type(&r, 600, "0000");
    type(&r, 650, "4822");
    CHECK(ui_pin_failures(&r.store) == 2 && !r.rt.core.pin_unlocked, "wrong PINs are counted in the F-RAM and do not unlock");
    for (unsigned i = 0; i < sizeof(r.rt.pad); i++) {
        CHECK(r.rt.pad[i] == 0, "the digits are wiped after enter");
    }
    type(&r, 700, "4821");
    CHECK(r.rt.core.pin_unlocked && ui_pin_failures(&r.store) == 0, "the right PIN unlocks and clears the count");
    ui_runtime_touch(&r.rt, 750, UI_ACT_JOG_ARM, 1);
    ui_runtime_touch(&r.rt, 800, UI_ACT_JOG_Z_MINUS, 1);
    CHECK(r.machine.jogs == 1 && r.machine.last_jog == UI_ACT_JOG_Z_MINUS, "once unlocked the jog reaches the machine");
    ui_runtime_touch(&r.rt, 850, UI_ACT_STOP, 1);
    CHECK(r.machine.stops == 1, "STOP never asked for the PIN");
}

static void failures_survive_a_restart(void)
{
    static rig_t r;
    memset(&r, 0, sizeof(r));
    open_store(&r);
    ui_pin_set(&r.store, "4821", k_salt);
    start(&r, 1);
    run(&r, 0, 300);
    type(&r, 350, "1111");
    type(&r, 400, "2222");
    type(&r, 450, "3333");
    CHECK(r.rt.core.pin_lockout_until > 450, "three wrong PINs lock the pad out");
    type(&r, 500, "4821");
    CHECK(!r.rt.core.pin_unlocked, "a right PIN is ignored during the lock-out");
    /* the power was cut: a new runtime on the same chip */
    start(&r, 1);
    CHECK(ui_pin_failures(&r.store) >= 3, "the counted failures are in the chip");
    CHECK(r.rt.core.pin_lockout_until > 0, "at start the lock-out begins at once");
    type(&r, 10, "4821");
    CHECK(!r.rt.core.pin_unlocked, "a power cut bought no new tries");
}

static void erase_with_zeros(void)
{
    static rig_t r;
    memset(&r, 0, sizeof(r));
    open_store(&r);
    ui_pin_set(&r.store, "4821", k_salt);
    memset(r.chip.chip, 0, sizeof(r.chip.chip)); /* the F-RAM erase of the brain */
    CHECK(ui_pin_state(&r.store) == UI_PIN_STATE_UNSET, "zeros over the chip read as no PIN");
    start(&r, 1);
    run(&r, 0, 300);
    type(&r, 350, "4821");
    CHECK(!r.rt.core.pin_unlocked, "with no PIN set nothing unlocks the panel");
    ui_runtime_touch(&r.rt, 400, UI_ACT_STOP, 1);
    CHECK(r.machine.stops == 1, "and STOP still works");
    CHECK(ui_pin_set(&r.store, "4821", k_salt) == UI_PIN_SET_OK, "the Studio sets it again");
    type(&r, 450, "4821");
    CHECK(r.rt.core.pin_unlocked, "and the new PIN works");
}

int main(void)
{
    hang_and_recover();
    stop_is_always_first();
    pin_chain();
    failures_survive_a_restart();
    erase_with_zeros();
    if (failures == 0) {
        printf("ui_runtime: PASS\n");
        return 0;
    }
    printf("ui_runtime: FAIL (%d)\n", failures);
    return 1;
}
