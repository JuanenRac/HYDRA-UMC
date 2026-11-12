// =============================================================================
// HYDRA-UMC Bootloader - Ed25519 signature check of an update (see update_signature.h)
// Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
// GPL-3.0-or-later - see repo root LICENSE
// =============================================================================
#include <string.h>

#include "bootloader_common.h"
#include "tweetnacl.h"
#include "update_signature.h"

// TweetNaCl links against this for key generation and signing, which the bootloader never does. A board has nothing it may sign with,
// so asking it for random bytes is a mistake that must not return plausible bytes.
void randombytes(unsigned char *buffer, unsigned long long length) {
    for (unsigned long long i = 0; i < length; i++) buffer[i] = 0;
    for (;;) { }
}

// The verification takes longer than the watchdog period at the clock of the bootloader: tweetnacl.c calls this once per scalar bit.
void tweetnacl_yield(void) {
    HAL_IWDG_Refresh(&hiwdg);
}

// The order of the base point, little endian: a signature whose second half is not below it is not canonical and is refused.
static const uint8_t ED25519_L[32] = {
    0xED, 0xD3, 0xF5, 0x5C, 0x1A, 0x63, 0x12, 0x58, 0xD6, 0x9C, 0xF7, 0xA2, 0xDE, 0xF9, 0xDE, 0x14,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10
};

static int ScalarIsReduced(const uint8_t s[32]) {
    for (int i = 31; i >= 0; i--) {
        if (s[i] < ED25519_L[i]) return 1;
        if (s[i] > ED25519_L[i]) return 0;
    }
    return 0; // equal to L
}

int UpdateSignature_Verify(const uint8_t *msg, uint32_t len, const uint8_t signature[UPDATE_SIGNATURE_SIZE],
                           const uint8_t public_key[UPDATE_PUBLIC_KEY_SIZE]) {
    if (msg == NULL || signature == NULL || public_key == NULL || len > UPDATE_SIGNATURE_MAX_MESSAGE) return 0;
    if (!ScalarIsReduced(signature + 32)) return 0;
    uint8_t signed_message[UPDATE_SIGNATURE_SIZE + UPDATE_SIGNATURE_MAX_MESSAGE];
    uint8_t opened[UPDATE_SIGNATURE_SIZE + UPDATE_SIGNATURE_MAX_MESSAGE];
    unsigned long long opened_length = 0;
    memcpy(signed_message, signature, UPDATE_SIGNATURE_SIZE);
    if (len > 0) memcpy(signed_message + UPDATE_SIGNATURE_SIZE, msg, len);
    int result = crypto_sign_open(opened, &opened_length, signed_message, UPDATE_SIGNATURE_SIZE + (unsigned long long)len, public_key);
    return result == 0 && opened_length == len;
}
