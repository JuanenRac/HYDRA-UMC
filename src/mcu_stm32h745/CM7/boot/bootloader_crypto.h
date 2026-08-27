// =============================================================================
// HYDRA-UMC Kinematic Brain Bootloader (CM7) - SHA-256 / HMAC-SHA256
// Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
// GPL-3.0 - see repo root LICENSE
//
// Ported verbatim from URTC's own proven bootloader_crypto.h (via HYDRA-
// UMC's own G474 port) - the SHA-256/HMAC algorithm has no chip-specific
// content at all.
// =============================================================================
#ifndef BOOTLOADER_CRYPTO_H
#define BOOTLOADER_CRYPTO_H

#include <stdint.h>

void hmac_sha256_flash_region(const uint8_t *key, uint32_t key_len,
                              uint32_t flash_addr, uint32_t len,
                              uint8_t out[32]);

uint8_t hmac_constant_time_compare(const uint8_t *a, const uint8_t *b, uint32_t len);

#endif // BOOTLOADER_CRYPTO_H
