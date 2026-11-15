/* =============================================================================
 * HYDRA-UMC Firmware - src/mcu_stm32h757/Common/touch_gt911.h
 * Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
 * GPL-3.0-or-later - see LICENSE
 * =============================================================================
 * touch_gt911.h - the touch input of the interface (HAL-free, host-testable): reads a Goodix GT911 through the two operations of an I2C bus (write and read of 7-bit addresses, the shape
 * of CM7/I2cBuses.c) and gives the interface one sample, pressed or released with a position in the pixels of the picture. The controller has 16-bit register addresses, sent high byte
 * first. The status register (0x814E) says in bit 7 that a new frame of points is ready and in bits 0-3 how many points; the first point (0x8150) is eight bytes: track id, X and Y as
 * little-endian 16-bit values, size. The frame is acknowledged by writing 0 to the status register, otherwise the controller does not report the next one. Only the first point is used (the
 * interface is a single-touch one: STOP, buttons and the jog arm). The raw position is turned into a position of the picture by the numbers of config/lvgl.cfg (swap, mirror, scale); a point
 * outside the raw size is refused and the previous sample stays. When no new frame is ready the previous sample is repeated, which is what the input of the library wants. The model on
 * the real panel is not confirmed; tools/touch_gt911.py is the Python twin. OFF by default: nothing calls this file (HYDRA_TOUCH_GT911 0). Evidence: simulator; nothing here has
 * run on a board.
 * ============================================================================= */
#ifndef HYDRA_UMC_TOUCH_GT911_H
#define HYDRA_UMC_TOUCH_GT911_H

#include <stdint.h>

#define GT911_ADDR_PRIMARY 0x5DU
#define GT911_ADDR_ALTERNATE 0x14U
#define GT911_REG_ID 0x8140U
#define GT911_REG_STATUS 0x814EU
#define GT911_REG_POINT0 0x8150U
#define GT911_POINT_BYTES 8U
#define GT911_MAX_POINTS 5U
#define GT911_SIZE_MAX 2048U

#define GT911_OK 0
#define GT911_NODATA 1 /* no new frame: the previous sample is given again */
#define GT911_E_ARG 2
#define GT911_E_CONFIG 3 /* an address that is not 0x5D or 0x14, a size outside 1..2048 */
#define GT911_E_BUS 4    /* the bus refused a transfer */
#define GT911_E_COUNT 5  /* the status says more points than the controller has */
#define GT911_E_RANGE 6  /* the point is outside the raw size */
#define GT911_E_ID 7     /* the chip does not answer as a GT911 */

typedef struct {
    uint8_t address7;
    uint16_t raw_width, raw_height; /* what the controller reports */
    uint16_t width, height;         /* the picture the position is mapped to */
    uint8_t swap_xy, mirror_x, mirror_y;
} Gt911Config;

typedef struct {
    uint8_t pressed;
    uint16_t x, y;
} TouchSample;

typedef struct {
    int (*write)(void *ctx, uint8_t address7, const uint8_t *data, uint16_t length);
    int (*read)(void *ctx, uint8_t address7, uint8_t *data, uint16_t length);
    void *ctx;
} Gt911Bus;

typedef struct {
    TouchSample last;
} Gt911State;

static inline int Gt911_Check(const Gt911Config *c)
{
    if (c == 0) return GT911_E_ARG;
    if (c->address7 != GT911_ADDR_PRIMARY && c->address7 != GT911_ADDR_ALTERNATE) return GT911_E_CONFIG;
    if (c->raw_width < 1U || c->raw_width > GT911_SIZE_MAX || c->raw_height < 1U || c->raw_height > GT911_SIZE_MAX) return GT911_E_CONFIG;
    if (c->width < 1U || c->width > GT911_SIZE_MAX || c->height < 1U || c->height > GT911_SIZE_MAX) return GT911_E_CONFIG;
    return GT911_OK;
}

/* A raw position as a position of the picture: swap the axes, mirror, then scale to the picture (rounded down, held inside it). */
static inline int Gt911_Map(const Gt911Config *c, uint16_t raw_x, uint16_t raw_y, uint16_t *x, uint16_t *y)
{
    uint32_t sx, sy, rw, rh;
    int rc = Gt911_Check(c);
    if (rc != GT911_OK) return rc;
    if (x == 0 || y == 0) return GT911_E_ARG;
    if (raw_x >= c->raw_width || raw_y >= c->raw_height) return GT911_E_RANGE;
    sx = raw_x;
    sy = raw_y;
    rw = c->raw_width;
    rh = c->raw_height;
    if (c->swap_xy) {
        uint32_t t = sx;
        sx = sy;
        sy = t;
        t = rw;
        rw = rh;
        rh = t;
    }
    if (c->mirror_x) sx = rw - 1UL - sx;
    if (c->mirror_y) sy = rh - 1UL - sy;
    *x = (uint16_t)((sx * c->width) / rw);
    *y = (uint16_t)((sy * c->height) / rh);
    return GT911_OK;
}

static inline int Gt911_Reg(const Gt911Bus *b, uint8_t address7, uint16_t reg, uint8_t *data, uint16_t length)
{
    uint8_t r[2];
    r[0] = (uint8_t)(reg >> 8);
    r[1] = (uint8_t)(reg & 0xFFU);
    if (b->write(b->ctx, address7, r, 2U) != 0) return GT911_E_BUS;
    return b->read(b->ctx, address7, data, length) == 0 ? GT911_OK : GT911_E_BUS;
}

static inline int Gt911_Ack(const Gt911Bus *b, uint8_t address7)
{
    const uint8_t frame[3] = {(uint8_t)(GT911_REG_STATUS >> 8), (uint8_t)(GT911_REG_STATUS & 0xFFU), 0U};
    return b->write(b->ctx, address7, frame, 3U) == 0 ? GT911_OK : GT911_E_BUS;
}

/* The chip answers its product id, "911" and a zero. */
static inline int Gt911_Probe(const Gt911Bus *b, const Gt911Config *c)
{
    uint8_t id[4];
    int rc = Gt911_Check(c);
    if (rc != GT911_OK) return rc;
    if (b == 0 || b->write == 0 || b->read == 0) return GT911_E_ARG;
    rc = Gt911_Reg(b, c->address7, GT911_REG_ID, id, 4U);
    if (rc != GT911_OK) return rc;
    return (id[0] == '9' && id[1] == '1' && id[2] == '1' && id[3] == 0U) ? GT911_OK : GT911_E_ID;
}

/* One poll: GT911_OK with a new sample, GT911_NODATA when no frame is ready (the previous sample is given again). A frame that was read is always acknowledged. */
static inline int Gt911_Poll(const Gt911Bus *b, const Gt911Config *c, Gt911State *s, TouchSample *out)
{
    uint8_t status, raw[GT911_POINT_BYTES];
    uint16_t x, y;
    unsigned count;
    int rc = Gt911_Check(c);
    if (rc != GT911_OK) return rc;
    if (b == 0 || b->write == 0 || b->read == 0 || s == 0 || out == 0) return GT911_E_ARG;
    *out = s->last;
    rc = Gt911_Reg(b, c->address7, GT911_REG_STATUS, &status, 1U);
    if (rc != GT911_OK) return rc;
    if ((status & 0x80U) == 0U) return GT911_NODATA;
    count = status & 0x0FU;
    if (count > GT911_MAX_POINTS) {
        (void)Gt911_Ack(b, c->address7);
        return GT911_E_COUNT;
    }
    if (count == 0U) {
        s->last.pressed = 0U;
    } else {
        rc = Gt911_Reg(b, c->address7, GT911_REG_POINT0, raw, GT911_POINT_BYTES);
        if (rc != GT911_OK) return rc;
        rc = Gt911_Map(c, (uint16_t)(raw[1] | ((unsigned)raw[2] << 8)), (uint16_t)(raw[3] | ((unsigned)raw[4] << 8)), &x, &y);
        if (rc != GT911_OK) {
            (void)Gt911_Ack(b, c->address7);
            return rc;
        }
        s->last.pressed = 1U;
        s->last.x = x;
        s->last.y = y;
    }
    *out = s->last;
    return Gt911_Ack(b, c->address7);
}

#endif /* HYDRA_UMC_TOUCH_GT911_H */
