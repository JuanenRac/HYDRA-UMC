// =============================================================================
// HYDRA-UMC Bootloader - Ed25519 signature check of an update
// Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
// GPL-3.0-or-later - see repo root LICENSE
// =============================================================================
// One function, the same file in the three bootloaders: is `signature` a valid Ed25519 (RFC 8032) signature of `msg` under a public key?
// The board holds only the public key (UPDATE_PUBLIC_KEY, bootloader_crypto.c), so nothing on it can sign an update. The messages that
// are verified are short (the 52-byte image message and the 80-byte downgrade message of image_header.h): the image itself is hashed with
// SHA-256 straight out of the flash while it arrives, and only that digest is signed. The curve arithmetic is TweetNaCl (tweetnacl.c,
// public domain, see THIRD_PARTY_NOTICES.md).
#ifndef UPDATE_SIGNATURE_H
#define UPDATE_SIGNATURE_H

#include <stdint.h>

#define UPDATE_SIGNATURE_SIZE   64u
#define UPDATE_PUBLIC_KEY_SIZE  32u
// the longest message UpdateSignature_Verify accepts
#define UPDATE_SIGNATURE_MAX_MESSAGE 96u

// 1 when the signature is valid, 0 for anything else (a wrong signature, a message that is too long, a scalar that is not reduced,
// a public key that is not a point). It takes a few hundred milliseconds to a second on the clock the bootloader runs on, and refreshes
// the watchdog while it works.
int UpdateSignature_Verify(const uint8_t *msg, uint32_t len, const uint8_t signature[UPDATE_SIGNATURE_SIZE],
                           const uint8_t public_key[UPDATE_PUBLIC_KEY_SIZE]);

#endif // UPDATE_SIGNATURE_H
