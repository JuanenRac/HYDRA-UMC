// =============================================================================
// HYDRA-UMC Bootloader - boot_update_glue.h: the part of the update flow that is the same in the three bootloaders
// Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
// GPL-3.0-or-later - see repo root LICENSE
//
// The Robot Controller Board (CAN), the Kinematic Brain CM4 (SPI) and the CM7 (mailbox) differ in how a frame arrives and is answered, not in what
// is done with the chunks of the signature, the header and the downgrade token, nor in how a verdict becomes the reason the host is told. That
// part lives here, once; each bootloader_protocol.c includes this file after the state it works on (update_signature, update_header,
// update_nonce, update_token and their chunk counters, update_session_counter) and after its own SendNonceHalf(). The file is a text include,
// not a separate translation unit, because it works on the static state of the protocol file. The same text is kept in the three boot folders
// (each target builds from its own); tests/test_update_glue_parity.py keeps them identical and checks that no bootloader defines these
// functions a second time.
// =============================================================================
#ifndef BOOT_UPDATE_GLUE_H
#define BOOT_UPDATE_GLUE_H

// A value that is different in every session, announced to the host in clear (OFS_DOWNGRADE_NONCE) and bound into the downgrade token, so that
// a token recorded in one session is useless in another. It is a freshness value, not a secret: it only has to differ from every nonce an
// attacker has seen a token for. This default mixes the state of this boot (tick, SysTick), the unique id of the chip and the number of
// sessions of this boot; a build whose chip has a hardware random generator should define Boot_SessionEntropy itself with it.
#ifndef UID_BASE
#define UID_BASE 0UL
#endif
__attribute__((weak)) void Boot_SessionEntropy(uint8_t out[16]) {
    uint32_t words[4];
    words[0] = HAL_GetTick();
    words[1] = SysTick->VAL;
#if UID_BASE != 0UL
    words[2] = *(volatile const uint32_t*)(UID_BASE) ^ *(volatile const uint32_t*)(UID_BASE + 8UL);
    words[3] = *(volatile const uint32_t*)(UID_BASE + 4UL);
#else
    words[2] = 0;
    words[3] = 0;
#endif
    memcpy(out, words, 16);
}

static void MakeSessionNonce(uint8_t nonce[IMAGE_NONCE_SIZE]) {
    uint8_t material[20];
    uint8_t digest[32];
    Boot_SessionEntropy(material);
    update_session_counter++;
    memcpy(material + 16, &update_session_counter, 4);
    sha256_buffer(material, sizeof(material), digest);
    memcpy(nonce, digest, IMAGE_NONCE_SIZE);
}

// The old flow let anybody on the bus switch the anti-rollback off with four constant bytes. That frame is now ignored: a downgrade
// needs the token of OFS_DOWNGRADE_TOKEN, an Ed25519 signature, under the update key, of this session's nonce, the installed and the new version and
// the digest of the image (update_verify.h). Kept as a function so the dispatch of the main loop stays as it was.
void HandleAuthorizeDowngrade(uint8_t *data) {
    (void)data;
}

// Eight frames of eight bytes, in order, right after START_UPDATE: the Ed25519 signature of the image (the frame kept its old name, OFS_HMAC_CHUNK).
void HandleHmacChunk(uint8_t *data) {
    if (!update_in_progress || update_failed) return;
    if (update_signature_chunks_received >= UPDATE_SIGNATURE_CHUNKS) return;
    memcpy(&update_signature[update_signature_chunks_received * 8], data, 8);
    update_signature_chunks_received++;
    update_last_activity_tick = HAL_GetTick();
}

// Eight frames of eight bytes, in order, right after START_UPDATE: the 64-byte header the signature was made over (image_header.h).
void HandleImageHeaderChunk(uint8_t *data) {
    if (!update_in_progress || update_failed) return;
    if (update_header_chunks_received >= UPDATE_HEADER_CHUNKS) return;
    memcpy(&update_header[update_header_chunks_received * 8], data, 8);
    update_header_chunks_received++;
    update_last_activity_tick = HAL_GetTick();
}

// The host asks for the nonce of this session, a half at a time (DLC=1, 0 or 1): the nonce is 16 bytes and a frame carries 8. Only while an
// update is in progress; the answer is the bytes of that half, and the host builds the downgrade token from both.
void HandleDowngradeNonceRequest(uint8_t *data, uint32_t dlc) {
    if (!update_in_progress || update_failed || dlc != 1 || data[0] > 1) return;
    SendNonceHalf(update_nonce + 8 * data[0]);
}

// Eight frames of eight bytes, optional: the signature that allows one downgrade. Nothing is believed until END_UPDATE checks it.
void HandleDowngradeTokenChunk(uint8_t *data) {
    if (!update_in_progress || update_failed) return;
    if (update_token_chunks_received >= UPDATE_TOKEN_CHUNKS) return;
    memcpy(&update_token[update_token_chunks_received * 8], data, 8);
    update_token_chunks_received++;
    update_last_activity_tick = HAL_GetTick();
}

static int UpdateSignatureUnderTheKey(const uint8_t *msg, uint32_t len, const uint8_t signature[IMAGE_SIGNATURE_SIZE], const uint8_t public_key[32], void *context) {
    (void)context;
    return UpdateSignature_Verify(msg, len, signature, public_key);
}

// What the host is told when the decision of update_verify.h is not OK.
static uint8_t VerifyFailReasonOf(UpdateVerdict_t verdict) {
    switch (verdict) {
        case UPDATE_VERDICT_INCOMPLETE:   return VERIFY_FAIL_REASON_INCOMPLETE;
        case UPDATE_VERDICT_CRC32:        return VERIFY_FAIL_REASON_CRC32;
        case UPDATE_VERDICT_SIGNATURE:    return VERIFY_FAIL_REASON_SIGNATURE;
        case UPDATE_VERDICT_UNKNOWN_KEY:  return VERIFY_FAIL_REASON_SIGNATURE;   // no key of the trust table made it
        case UPDATE_VERDICT_COUNTER:      return VERIFY_FAIL_REASON_ROLLBACK;    // the signed counter is below the floor
        case UPDATE_VERDICT_HARDWARE_ID:  return VERIFY_FAIL_REASON_HARDWARE_ID;
        case UPDATE_VERDICT_ROLLBACK:     return VERIFY_FAIL_REASON_ROLLBACK;
        default:                          return VERIFY_FAIL_REASON_HEADER;
    }
}

#endif // BOOT_UPDATE_GLUE_H
