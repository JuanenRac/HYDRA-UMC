/* =============================================================================
 * HYDRA-UMC Firmware - src/ui_h757/core/ui_runtime.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * The runtime of the interface of the STM32H757 without hardware: it ties the view-model (ui_core.h), the host side of the Jetson link (ui_link.h) and the store of the operator
 * PIN (ui_pin.h) to the ports a board gives it, and turns the effects of the view-model into calls of those ports. The firmware that draws (LTDC, DSI, the library) and the tasks
 * are written against this file; the host tests run it against fakes. HAL-free, no malloc.
 *
 * THE STOP PATH: ui_runtime_touch with the STOP action calls send_stop BEFORE anything else and touches neither the SPI link nor the PIN nor the pad, so no drawing, no heavy
 * hash and no Jetson that does not answer can delay it. That it also runs fast enough on a scheduler is a property of the task priorities (tools/ui_sched_sim.py models them and
 * the bench measures them); this file only keeps the path short and free of waiting.
 * Nothing here has run on a board. Evidence: simulator (tests/test_ui_runtime_c.py).
 */
#ifndef UI_RUNTIME_H
#define UI_RUNTIME_H

#include "ui_core.h"
#include "ui_link.h"
#include "ui_pin.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_STOP_REASON_OPERATOR 0x01u /* the reason byte of the stop frame: docs/ESTOP_CAN.md */
#define UI_SETTING_LISTEN_ONLY 0u
#define UI_SETTING_LANGUAGE 1u
#define UI_PAD_MAX_DIGITS 12u

typedef struct {
    /* The SPI transaction with the Jetson, or 0 when the board has none: then the panel runs degraded from the end of the start-up grace. */
    ui_link_transact_t spi_transact;
    void *spi_context;
    /* What the effects of the view-model become. Each may be 0 (nothing is done). */
    void (*send_stop)(void *context, uint8_t reason);
    void (*send_release)(void *context);
    void (*send_jog)(void *context, uint8_t action); /* a ui_action_t of a jog */
    void (*save_setting)(void *context, uint8_t key, uint8_t value);
    void *context;
} ui_runtime_ports_t;

typedef struct {
    ui_state_t core;
    ui_link_master_t link;
    ui_pin_store_t pin;
    ui_runtime_ports_t ports;
    uint8_t pad[UI_PAD_MAX_DIGITS + 1u];
    uint8_t pad_length;
    uint32_t pad_max_digits;
    uint32_t stops_sent;
} ui_runtime_t;

/* Starts the runtime. `picture` is the buffer for the camera picture (capacity bytes). The PIN store is the caller's, already initialised. At start the number of wrong PINs the store
 * has counted is told to the view-model (a power cut buys no new tries). */
void ui_runtime_init(ui_runtime_t *rt, const ui_config_t *cfg, const ui_runtime_ports_t *ports, const ui_pin_store_t *pin, uint8_t *picture, uint32_t capacity);

/* Runs what is due at this time: the link poll (the status is the heartbeat), and a tick of the view-model. */
void ui_runtime_poll(ui_runtime_t *rt, uint64_t now_ms);

/* A touch on a button: the STOP action goes out at once; the others go through the view-model. `down` is 1 for a press and 0 for a release. */
void ui_runtime_touch(ui_runtime_t *rt, uint64_t now_ms, int action, int down);

/* Inputs from the stack bus and from the brain. */
void ui_runtime_bus(ui_runtime_t *rt, uint64_t now_ms, int bus_off, int tx_errors, int rx_errors);
void ui_runtime_estop(ui_runtime_t *rt, uint64_t now_ms, int latched);
void ui_runtime_motion(ui_runtime_t *rt, uint64_t now_ms, int moving);
void ui_runtime_navigate(ui_runtime_t *rt, uint64_t now_ms, int screen);

/* The PIN pad: a digit (0-9), clear, enter. Enter checks the digits against the store and tells the view-model right or wrong; the digits are wiped at once. */
void ui_runtime_pad_digit(ui_runtime_t *rt, int digit);
void ui_runtime_pad_clear(ui_runtime_t *rt);
void ui_runtime_pad_enter(ui_runtime_t *rt, uint64_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* UI_RUNTIME_H */
