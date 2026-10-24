// =============================================================================
// HYDRA-UMC Kinematic Brain Bootloader (CM4) - SHA-256 / HMAC-SHA256
// Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
// GPL-3.0-or-later - see repo root LICENSE
// =============================================================================
#ifndef BOOTLOADER_CRYPTO_H
#define BOOTLOADER_CRYPTO_H

#include <stdint.h>

void hmac_sha256_flash_region(const uint8_t *key, uint32_t key_len,
                              uint32_t flash_addr, uint32_t len,
                              uint8_t out[32]);

// The same HMAC over `prefix` (a buffer in RAM) followed by a flash region, as ONE message: the tag of a signed image covers its
// 64-byte header and then the image, which is in flash. With prefix_len == 0 it is hmac_sha256_flash_region.
void hmac_sha256_prefixed_flash_region(const uint8_t *key, uint32_t key_len,
                                       const uint8_t *prefix, uint32_t prefix_len,
                                       uint32_t flash_addr, uint32_t len,
                                       uint8_t out[32]);

// HMAC-SHA256 over a buffer in RAM (the downgrade token is the HMAC of a short message).
void hmac_sha256_buffer(const uint8_t *key, uint32_t key_len,
                        const uint8_t *msg, uint32_t msg_len,
                        uint8_t out[32]);

uint8_t hmac_constant_time_compare(const uint8_t *a, const uint8_t *b, uint32_t len);

#endif // BOOTLOADER_CRYPTO_H
