/* =============================================================================
 * HYDRA-UMC Firmware - src/ui_h757/core/ui_pin.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * The store of the operator PIN in C: the twin of tools/ui_pin.py (read its header first). Two slots of 72 bytes in the external F-RAM, each with a salted PBKDF2-HMAC-SHA256
 * hash of the digits (never the digits), a failure counter and a CRC; the valid slot with the newer sequence wins; the failure counter is written BEFORE the comparison; a
 * record that cannot be read never lets a PIN in. The bytes of a record are those of the Python reference (tests/test_ui_pin_c.py compares them).
 *
 * No HAL, no malloc, no global state: the F-RAM is reached through two functions the caller gives. The view-model (ui_core.h) never sees the digits; the pad calls ui_pin_verify
 * and tells the core PIN_OK or PIN_BAD. Nothing here has run on a board. Evidence: simulator (host tests); the cost of the hash on the Cortex-M7 is an estimate (tools/ui_pin_cost.py).
 */
#ifndef UI_PIN_H
#define UI_PIN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UI_PIN_RECORD_BYTES 72u
#define UI_PIN_SLOTS 2u
#define UI_PIN_SALT_BYTES 16u
#define UI_PIN_HASH_BYTES 32u

typedef enum { UI_PIN_OK = 0, UI_PIN_BAD, UI_PIN_UNAVAILABLE } ui_pin_result_t;
typedef enum { UI_PIN_STATE_UNSET = 0, UI_PIN_STATE_SET, UI_PIN_STATE_CORRUPT } ui_pin_state_t;
typedef enum { UI_PIN_SET_OK = 0, UI_PIN_SET_BAD_PIN, UI_PIN_SET_BAD_SALT, UI_PIN_SET_WRITE_FAILED } ui_pin_set_result_t;

/* The F-RAM as the caller gives it. read and write return 0 when all the bytes were moved and nonzero otherwise (a write cut by the power leaves the bytes written so far). */
typedef struct {
    int (*read)(void *context, uint32_t offset, uint8_t *data, uint32_t length);
    int (*write)(void *context, uint32_t offset, const uint8_t *data, uint32_t length);
    void *context;
} ui_pin_ports_t;

typedef struct {
    ui_pin_ports_t ports;
    uint32_t iterations;  /* of a PIN set from now on (the record keeps its own) */
    uint32_t min_digits;
    uint32_t max_digits;
} ui_pin_store_t;

void ui_pin_init(ui_pin_store_t *store, const ui_pin_ports_t *ports, uint32_t iterations, uint32_t min_digits, uint32_t max_digits);

/* unset: both slots are erased (all 0xFF); set: a valid record; corrupt: something is there that cannot be read. */
ui_pin_state_t ui_pin_state(const ui_pin_store_t *store);

/* The failure counter of the valid record, 0 when there is none. */
uint32_t ui_pin_failures(const ui_pin_store_t *store);

/* Sets the PIN (from the Studio, not from the panel). Refuses a PIN of the wrong length, not made of ASCII digits or of one repeated digit. */
ui_pin_set_result_t ui_pin_set(ui_pin_store_t *store, const char *pin, const uint8_t salt[UI_PIN_SALT_BYTES]);

/* The reset from the Studio: writes 0xFF over both slots. Returns 0 on success. */
int ui_pin_erase(ui_pin_store_t *store);

/* OK, BAD or UNAVAILABLE. The failure counter goes to the F-RAM before the comparison and back to zero after a right PIN. */
ui_pin_result_t ui_pin_verify(const ui_pin_store_t *store, const char *pin);

/* Building blocks, public so the host tests can check them against the reference: */
void ui_pbkdf2_sha256(const uint8_t *password, uint32_t password_length, const uint8_t *salt, uint32_t salt_length, uint32_t iterations, uint8_t out[32]);
uint32_t ui_crc32(const uint8_t *data, uint32_t length);
#ifdef UI_PIN_COUNT_BLOCKS
extern uint32_t ui_sha256_blocks; /* compressions done, for the estimate of the cost (host tests only) */
#endif

#ifdef __cplusplus
}
#endif

#endif /* UI_PIN_H */
