/* =============================================================================
 * HYDRA-UMC Firmware - src/ui_h757/core/ui_core.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * The view-model of the interface of the STM32H757 in C: what the panel shows and which touches it believes. It is the twin of tools/ui_model.py; tests/test_ui_core.py
 * replays every scenario of src/ui_h757/spec/scenarios.json through both and compares the trace line by line.
 *
 * No HAL, no clock, no display, no bus, no malloc and no global state: an event carries the time, the answer is a list of effects (what the firmware must do). The
 * caller owns one ui_state_t. The rules that never change: STOP is allowed in every mode and goes out on the first touch; with listen-only on nothing but STOP and the
 * local settings works; with the Jetson not answering nothing moves and the stop is not released unless a switch of config/ui.cfg says so.
 * Nothing here has run on a board. Evidence: simulator (host tests).
 */
#ifndef UI_CORE_H
#define UI_CORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { UI_MODE_BOOT = 0, UI_MODE_NORMAL, UI_MODE_DEGRADED, UI_MODE_ALARM, UI_MODE_STOPPED } ui_mode_t;
typedef enum { UI_LINK_STARTING = 0, UI_LINK_OK, UI_LINK_SILENT, UI_LINK_SPI_FAULT } ui_link_t;
typedef enum { UI_BUS_OK = 0, UI_BUS_WARN, UI_BUS_OFF } ui_bus_t;
typedef enum {
    UI_BANNER_NORMAL = 0, UI_BANNER_BOOT, UI_BANNER_DEGRADED, UI_BANNER_SPI_FAULT, UI_BANNER_BUS_OFF, UI_BANNER_STOPPED, UI_BANNER_LISTEN_ONLY, UI_BANNER_ALARM,
    UI_BANNER_STOP_UNCONFIRMED, UI_BANNER_LOCKED
} ui_banner_t;

typedef enum {
    UI_ACT_STOP = 0, UI_ACT_RELEASE, UI_ACT_JOG_ARM, UI_ACT_JOG_X_PLUS, UI_ACT_JOG_X_MINUS, UI_ACT_JOG_Y_PLUS, UI_ACT_JOG_Y_MINUS, UI_ACT_JOG_Z_PLUS,
    UI_ACT_JOG_Z_MINUS, UI_ACT_ACK_ALARM, UI_ACT_TOGGLE_LISTEN_ONLY, UI_ACT_NEXT_LANGUAGE, UI_ACT_COUNT
} ui_action_t;

typedef enum {
    UI_REFUSED_LISTEN_ONLY = 0, UI_REFUSED_NOT_STOPPED, UI_REFUSED_STOPPED, UI_REFUSED_DEGRADED, UI_REFUSED_BUS_FAULT, UI_REFUSED_NOT_ARMED, UI_REFUSED_ALARM,
    UI_REFUSED_STARTING, UI_REFUSED_LOCKED
} ui_refusal_t;

typedef enum {
    UI_AL_JETSON_LOST = 0, UI_AL_SPI_LINK, UI_AL_CAN_ERRORS, UI_AL_CAN_BUS_OFF, UI_AL_STOP_UNCONFIRMED, UI_AL_MOTION_AFTER_STOP, UI_AL_ZONE_WARNING, UI_AL_ZONE_DANGER,
    UI_AL_ZONE_INHIBITED, UI_AL_ZONE_FAULT, UI_AL_COUNT
} ui_alarm_t;

typedef enum {
    UI_EV_TICK = 0, UI_EV_HEARTBEAT, UI_EV_SPI_ERROR, UI_EV_SPI_OK, UI_EV_CAN, UI_EV_ESTOP, UI_EV_MOTION, UI_EV_NAV, UI_EV_TOUCH_DOWN, UI_EV_TOUCH_UP, UI_EV_PIN_OK, UI_EV_PIN_BAD
} ui_event_kind_t;

typedef enum {
    UI_FX_STOP = 0,      /* send the stop to the brain (the first touch; repeated touches repeat it) */
    UI_FX_RELEASE,       /* ask the brain to end the stop */
    UI_FX_JOG,           /* a = the action (UI_ACT_JOG_*) */
    UI_FX_JOG_ARMED,     /* the jog is armed for jog_arm_ms */
    UI_FX_LISTEN,        /* a = 1 on, 0 off: remember the setting */
    UI_FX_LANGUAGE,      /* a = the new language index */
    UI_FX_ACK,           /* silence the alarms on the screen */
    UI_FX_REFUSED,       /* a = the action, b = the reason (ui_refusal_t): show it */
    UI_FX_ALARM_ON,      /* a = ui_alarm_t */
    UI_FX_ALARM_OFF,     /* a = ui_alarm_t */
    UI_FX_HOLD_CANCEL,
    UI_FX_UNLOCKED,      /* the operator PIN was right */
    UI_FX_LOCKED,        /* the panel locked itself after the idle time */
    UI_FX_PIN_WRONG,     /* a wrong PIN */
    UI_FX_PIN_LOCKOUT    /* too many wrong PINs: the pad is locked out for pin_lockout_ms */
} ui_effect_kind_t;

#define UI_ZONE_UNKNOWN 5u
#define UI_SCREEN_COUNT 9u
#define UI_LANGUAGE_COUNT 8u
#define UI_MAX_EFFECTS 24u
#define UI_NO_ACTION (-1)

typedef struct {
    uint32_t jetson_timeout_ms;
    uint32_t jetson_boot_grace_ms;
    uint32_t spi_fault_after_errors;
    uint32_t stop_confirm_ms;
    uint32_t stop_motion_ms;
    uint32_t release_hold_ms;
    uint32_t listen_off_hold_ms;
    uint32_t jog_arm_ms;
    uint32_t can_warn_errors;
    uint32_t degraded_allow_release;
    uint32_t degraded_allow_motion;
    uint32_t operator_pin_enabled; /* optional operator PIN, off by default; STOP never asks for it */
    uint32_t pin_max_failures;
    uint32_t pin_lockout_ms;
    uint32_t pin_idle_ms;
} ui_config_t;

typedef struct {
    uint8_t kind; /* ui_effect_kind_t */
    uint8_t a;
    uint8_t b;
} ui_effect_t;

typedef struct {
    ui_config_t cfg;
    uint64_t now;
    uint8_t seen_heartbeat;
    uint64_t last_heartbeat;
    uint8_t zone_level;
    uint32_t spi_error_run;
    uint32_t spi_errors;
    uint8_t can_off;
    uint8_t can_tx;
    uint8_t can_rx;
    uint8_t latched;
    uint64_t latched_since;
    uint8_t stop_pending;
    uint64_t stop_sent;
    uint8_t moving;
    uint8_t listen_only;
    uint64_t jog_armed_until; /* 0 = not armed */
    uint8_t screen;
    uint8_t language;
    int8_t hold_action;
    uint64_t hold_start;
    uint16_t acked;
    uint8_t pin_unlocked;
    uint32_t pin_failures;
    uint64_t pin_lockout_until;
    uint64_t last_touch;
    /* derived after every event */
    uint8_t link;
    uint8_t bus;
    uint16_t alarms;
    uint8_t mode;
    uint8_t banner;
    uint16_t caps;
    ui_effect_t fx[UI_MAX_EFFECTS];
    uint8_t fx_count;
} ui_state_t;

/* The state at power-up, with the values of the configuration copied. The derived fields are computed. */
void ui_init(ui_state_t *s, const ui_config_t *cfg);

/* Feeds one event; the effects are in s->fx[0 .. s->fx_count-1] until the next call. Time never goes back.
 *   TICK: nothing; HEARTBEAT: a = zone level (0-5, anything else reads as unknown); SPI_ERROR, SPI_OK: nothing;
 *   CAN: a = bus off, b = transmit errors, c = receive errors; ESTOP: a = latched; MOTION: a = moving; NAV: a = screen;
 *   TOUCH_DOWN, TOUCH_UP: a = the action; PIN_OK, PIN_BAD: the port says the PIN typed was right or wrong (the digits never enter the core). Returns the number of effects. */
uint8_t ui_event(ui_state_t *s, uint64_t now, ui_event_kind_t kind, int32_t a, int32_t b, int32_t c);

/* Why the action is refused now (a ui_refusal_t), or -1 when it is allowed. */
int ui_refusal(const ui_state_t *s, int action);

/* Whether the operator PIN is on and has not been given (or has expired). */
int ui_locked(const ui_state_t *s);

/* Whether the jog is armed now. */
int ui_jog_armed(const ui_state_t *s);

/* The worst level of the active alarms: 0 none, 1 warning, 2 danger, 3 fault. */
int ui_worst_level(const ui_state_t *s);

#ifdef __cplusplus
}
#endif

#endif /* UI_CORE_H */
