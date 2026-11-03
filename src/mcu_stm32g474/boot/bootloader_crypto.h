// =============================================================================
// HYDRA-UMC Robot Controller Board Bootloader - SHA-256
// Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
// GPL-3.0-or-later - see repo root LICENSE
//
// There is no key in this file or in its header: the digest of an image is what the Ed25519 signature covers, and the digest of the installed
// application is what the metadata keeps to notice a damaged flash.
// =============================================================================
#ifndef BOOTLOADER_CRYPTO_H
#define BOOTLOADER_CRYPTO_H

#include <stdint.h>

// Plain SHA-256 over `prefix` (a buffer in RAM, may be empty) followed by a flash region (may be empty), as ONE message: the digest of a signed image
// covers its 64-byte header and then the image, which is in flash. With prefix_len == 0 it is the SHA-256 of the flash region. The flash is read
// byte by byte (the backup slot does not fit in RAM) and the watchdog is refreshed while it works.
void sha256_prefixed_flash_region(const uint8_t *prefix, uint32_t prefix_len,
                                  uint32_t flash_addr, uint32_t len,
                                  uint8_t out[32]);

// SHA-256 over a buffer in RAM (the nonce of an update session is made with it).
void sha256_buffer(const uint8_t *msg, uint32_t msg_len, uint8_t out[32]);

// XOR-accumulate rather than an early-exit compare - an early exit leaks how many leading bytes matched through timing.
uint8_t digest_constant_time_compare(const uint8_t *a, const uint8_t *b, uint32_t len);

#endif // BOOTLOADER_CRYPTO_H
