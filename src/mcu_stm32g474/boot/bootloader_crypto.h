// =============================================================================
// HYDRA-UMC Robot Controller Board Bootloader - SHA-256 / HMAC-SHA256
// Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
// GPL-3.0-or-later - see repo root LICENSE
//
// Ported verbatim from URTC's own proven bootloader_crypto.h - the SHA-256/
// HMAC algorithm itself has no chip-specific content at all.
// =============================================================================
#ifndef BOOTLOADER_CRYPTO_H
#define BOOTLOADER_CRYPTO_H

#include <stdint.h>

// HMAC-SHA256 over an arbitrary flash region, read byte-by-byte rather than
// requiring the whole thing in RAM at once (the backup slot is 236KB; RAM
// here is 96KB total, most of it needed for the page buffer + HAL/app
// state) - the flash controller supports direct addressed reads with no
// special unlock needed for that.
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

// XOR-accumulate rather than an early-exit compare - an early exit leaks
// how many leading bytes matched through timing, which matters for a
// signature check even on a small embedded target listening on a shared bus.
uint8_t hmac_constant_time_compare(const uint8_t *a, const uint8_t *b, uint32_t len);

#endif // BOOTLOADER_CRYPTO_H
