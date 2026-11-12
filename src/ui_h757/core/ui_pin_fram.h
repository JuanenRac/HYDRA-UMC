/* =============================================================================
 * HYDRA-UMC Firmware - src/ui_h757/core/ui_pin_fram.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * The ports of the PIN store (ui_pin.h) over the F-RAM driver of the brain (src/mcu_stm32h757/CM4/fram_i2c.h): the two slots of the PIN live at the end of the chip, just below the
 * 64 bytes that the self-test of docs/FRAM_ERASE.md uses as scratch. The F-RAM erase of the brain writes zeros over the whole chip; the store reads that as "no PIN set" (never as a
 * PIN accepted). Header only, HAL-free: the bus layer is whatever the FramI2cBus_t of the build gives, a fake one on the host. Nothing here has run on a board.
 */
#ifndef UI_PIN_FRAM_H
#define UI_PIN_FRAM_H

#include "fram_i2c.h"
#include "ui_pin.h"

/* 8192 bytes of chip - 64 bytes of self-test scratch - 144 bytes of the two slots. */
#define UI_PIN_FRAM_BASE (FRAM_I2C_SIZE - FRAM_SELFTEST_BYTES - UI_PIN_SLOTS * UI_PIN_RECORD_BYTES)

typedef struct {
    FramI2c_t *fram;
} ui_pin_fram_t;

static inline int ui_pin_fram_read(void *context, uint32_t offset, uint8_t *data, uint32_t length)
{
    return FramI2c_Read(((ui_pin_fram_t *)context)->fram, UI_PIN_FRAM_BASE + offset, data, length);
}

/* FramI2c_Write reads every chunk back, so a write that returns 0 is one the chip holds. */
static inline int ui_pin_fram_write(void *context, uint32_t offset, const uint8_t *data, uint32_t length)
{
    return FramI2c_Write(((ui_pin_fram_t *)context)->fram, UI_PIN_FRAM_BASE + offset, data, length);
}

static inline void ui_pin_fram_ports(ui_pin_ports_t *ports, ui_pin_fram_t *adapter)
{
    ports->read = ui_pin_fram_read;
    ports->write = ui_pin_fram_write;
    ports->context = adapter;
}

#endif /* UI_PIN_FRAM_H */
