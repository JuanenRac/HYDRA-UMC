// =============================================================================
// HYDRA-UMC Kinematic Brain Bootloader (CM7) - mailbox-relayed protocol
// handlers, application validation, and jump
// Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
// GPL-3.0-or-later - see repo root LICENSE
//
// Ported from URTC's own proven bootloader_protocol.c via HYDRA-UMC's own
// G474 port. The one real structural difference: every OTHER bootloader in
// this project owns a physical bus (FDCAN1, or SPI1 on CM4) it sends/
// receives frames on directly. This core owns neither - IPC_SendResponse()
// below is the only "transmit" primitive, and it writes into the shared
// mailbox (../../Common/ipc_mailbox.h) instead of a peripheral FIFO, for
// CM4's own gateway loop to relay onward over whichever bus the original
// request actually arrived on (SPI1 from the CM5, in every case that
// reaches this core at all - see docs/architecture.md section 3).
// =============================================================================
#include "stm32h7xx_hal.h"
#include <string.h>
#include "bootloader_common.h"
#include "bootloader_protocol.h"
#include "bootloader_crypto.h"
#include "bootloader_flash.h"
#include "update_signature.h"
#include "update_verify.h"
#include "ipc_mailbox.h"

// Blocks (with IWDG refresh) until CM4 has taken IPC_HSEM_RESP_ID's slot -
// i.e. until the PREVIOUS response this core posted has actually been
// consumed - before writing a new one. Single-producer/single-consumer:
// this core is the only writer of .resp, CM4 is the only reader, so "the
// semaphore is free to take" is equivalent to "CM4 finished reading the
// last one", the same one-shot-per-post handshake every other bootloader's
// CAN_WaitForFreeMailbox() provides for a hardware TX FIFO slot instead.
static void IPC_SendResponse(uint8_t frame_type, const uint8_t *data, uint8_t dlc) {
    uint32_t wait_start = HAL_GetTick();
    while (HAL_HSEM_IsSemTaken(IPC_HSEM_RESP_ID)) {
        HAL_IWDG_Refresh(&hiwdg);
        if (HAL_GetTick() - wait_start > 100) return; // CM4 not draining - drop rather than block indefinitely, same "best-effort" reasoning as every other tier's own send helpers
    }
    IPC_MAILBOX->resp.frame_type = frame_type;
    IPC_MAILBOX->resp.dlc = dlc;
    memset((void*)IPC_MAILBOX->resp.payload, 0, 8);
    if (dlc > 0) memcpy((void*)IPC_MAILBOX->resp.payload, data, dlc);
    // SRAM4 writes above are Normal (bufferable) memory; the HSEM take just
    // below is a peripheral (Device) write, with no ordering guarantee
    // between the two bus types without an explicit barrier. Without this
    // __DSB(), CM4 could observe IPC_HSEM_RESP_ID taken and read a stale or
    // torn .resp frame before the writes above have actually landed in
    // SRAM4 - same "flush shared data before signaling" requirement as
    // CM4's own Relay_ToCM7() write side (../../CM4/boot/bootloader_
    // protocol.c), mirrored here for this mailbox's other direction.
    __DSB();
    HAL_HSEM_FastTake(IPC_HSEM_RESP_ID); // claims the slot - CM4 reads it, then releases, which is what the wait loop above watches for next time
}

void CAN_SendStatus(uint8_t status) {
    uint8_t d[1] = {status};
    IPC_SendResponse(OFS_STATUS, d, 1);
}

static void CAN_SendVerifyFailReason(uint8_t reason) {
    uint8_t d[2] = {STATUS_VERIFY_FAIL, reason};
    IPC_SendResponse(OFS_STATUS, d, 2);
}

static void CAN_SendPageAck(uint32_t page_index) {
    uint8_t d[4] = {
        (uint8_t)(page_index >> 24), (uint8_t)(page_index >> 16),
        (uint8_t)(page_index >> 8), (uint8_t)(page_index)
    };
    IPC_SendResponse(OFS_PAGE_ACK, d, 4);
}

void CAN_SendHeartbeat(uint8_t status, uint8_t progress_percent) {
    if (HAL_HSEM_IsSemTaken(IPC_HSEM_RESP_ID)) return; // best-effort, never blocks - same as every other tier's own heartbeat sender
    uint8_t d[2] = {status, progress_percent};
    IPC_SendResponse(OFS_HEARTBEAT, d, 2);
}

void HandleVersionQuery(void) {
    FirmwareMetadata_t meta;
    uint8_t have_meta = Metadata_Read(&meta);
    uint32_t hw_id = have_meta ? meta.hardware_id : 0;
    uint16_t ver_major = have_meta ? (uint16_t)meta.version_major : 0;
    uint8_t ver_minor = have_meta ? (uint8_t)meta.version_minor : 0;

    uint8_t d[8];
    d[0] = 0x01;
    d[1] = (uint8_t)(hw_id >> 24); d[2] = (uint8_t)(hw_id >> 16);
    d[3] = (uint8_t)(hw_id >> 8);  d[4] = (uint8_t)(hw_id);
    d[5] = (uint8_t)(ver_major >> 8); d[6] = (uint8_t)(ver_major);
    d[7] = ver_minor;
    IPC_SendResponse(OFS_VERSION_RESPONSE, d, 8);

    uint8_t d2[3] = {BOOTLOADER_VERSION_MAJOR, BOOTLOADER_VERSION_MINOR, BOOTLOADER_VERSION_PATCH};
    IPC_SendResponse(OFS_BOOTLOADER_VERSION, d2, 3);
}

// This core has no bus/error-counter register of its own to read (the
// mailbox isn't a fault-confining protocol the way CAN is) - answers with
// zeros rather than omitting the response entirely, so a querying host
// still gets SOME reply instead of a silent timeout it can't tell apart
// from "this core is dead".
void HandleErrorCounterQuery(void) {
    uint8_t d[2] = {0, 0};
    IPC_SendResponse(OFS_ERROR_COUNTERS_RESPONSE, d, 2);
}

static uint8_t StackPointerInValidRAM(uint32_t sp) {
    // CM7's own D1-domain AXI SRAM: 512KB @ 0x24000000 (see ../STM32H745
    // ZITx_CM7_APP.ld) - this core's only private RAM region.
    return (sp >= D1_AXISRAM_BASE + 0x400UL && sp <= D1_AXISRAM_BASE + 0x80000UL);
}

uint8_t ApplicationIsValid(void) {
    FirmwareMetadata_t meta;
    if (!Metadata_Read(&meta)) {
        uint32_t app_stack = *(volatile uint32_t*)MAIN_APP_ADDR;
        if (!StackPointerInValidRAM(app_stack)) return 0;

        FirmwareMetadata_t adopted = {0};
        adopted.magic = METADATA_MAGIC_VALID;
        adopted.state = META_STATE_APP_VALID;
        adopted.hardware_id = THIS_HARDWARE_ID;
        adopted.version_major = FIRMWARE_VERSION_MAJOR;
        adopted.version_minor = FIRMWARE_VERSION_MINOR;
        adopted.version_patch = FIRMWARE_VERSION_PATCH;
        adopted.size = APP_MAX_SIZE;
        uint32_t crc = 0xFFFFFFFFUL;
        crc = CRC32_Update(crc, (const uint8_t*)MAIN_APP_ADDR, APP_MAX_SIZE);
        adopted.crc32 = CRC32_Finalize(crc);
        hmac_sha256_flash_region(HMAC_KEY, 32, MAIN_APP_ADDR, APP_MAX_SIZE, adopted.hmac);
        return Metadata_EraseAndWrite(&adopted) ? 1 : 0;
    }

    if (meta.hardware_id != THIS_HARDWARE_ID) return 0;
    if (meta.size == 0 || meta.size > APP_MAX_SIZE) return 0;

    if (meta.state == META_STATE_COPY_PENDING) {
        CAN_SendStatus(STATUS_COPYING);
        if (!Flash_CopyRegion(MAIN_APP_ADDR, BACKUP_APP_ADDR, meta.size)) {
            return 0;
        }
        FirmwareMetadata_t done = meta;
        done.state = META_STATE_APP_VALID;
        if (!Metadata_EraseAndWrite(&done)) return 0;
        meta = done;
    }

    uint32_t crc = 0xFFFFFFFFUL;
    crc = CRC32_Update(crc, (const uint8_t*)MAIN_APP_ADDR, meta.size);
    crc = CRC32_Finalize(crc);
    if (crc != meta.crc32) return 0;

    uint8_t actual_hmac[32];
    hmac_sha256_flash_region(HMAC_KEY, 32, MAIN_APP_ADDR, meta.size, actual_hmac);
    if (!hmac_constant_time_compare(actual_hmac, meta.hmac, 32)) return 0;

    return 1;
}

void JumpToApplication(void) {
    typedef void (*pFunction)(void);
    uint32_t app_stack = *(volatile uint32_t*)MAIN_APP_ADDR;
    uint32_t app_reset_vector = *(volatile uint32_t*)(MAIN_APP_ADDR + 4);

    if (!StackPointerInValidRAM(app_stack) ||
        (app_stack & 0x3UL) != 0 ||
        (app_reset_vector & 0x1UL) == 0 ||
        app_reset_vector < MAIN_APP_ADDR ||
        app_reset_vector >= MAIN_APP_ADDR + APP_MAX_SIZE) {
        return;
    }

    HAL_RCC_DeInit();
    HAL_DeInit();
    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL = 0;
    SCB->ICSR |= SCB_ICSR_PENDSTCLR_Msk;

    __disable_irq();
    SCB->VTOR = MAIN_APP_ADDR;
    __DSB();
    __ISB();

    pFunction app_entry = (pFunction)app_reset_vector;
    __set_MSP(app_stack);
    app_entry();
}

// -----------------------------------------------------------------------
// Update flow - identical state machine to every other bootloader in this
// project (see G474's own bootloader_protocol.c for the more heavily-
// commented original this was ported from). Backup slot only until fully
// verified; main slot untouched until then.
// -----------------------------------------------------------------------
uint32_t update_total_size = 0;
static uint32_t update_declared_hw_id = 0;
uint32_t update_bytes_received = 0;
static uint32_t update_running_crc = 0xFFFFFFFFUL;
static uint8_t  update_signature[IMAGE_SIGNATURE_SIZE];     // the Ed25519 signature of the image (eight OFS_HMAC_CHUNK frames)
static uint8_t  update_signature_chunks_received = 0;
static uint8_t  update_header[IMAGE_HEADER_SIZE];          // the signed header (eight OFS_IMAGE_HEADER frames)
static uint8_t  update_header_chunks_received = 0;
static uint8_t  update_nonce[IMAGE_NONCE_SIZE];            // the nonce of this session; a downgrade token is bound to it
static uint8_t  update_token[IMAGE_SIGNATURE_SIZE];        // the optional downgrade token, a signature too (eight OFS_DOWNGRADE_TOKEN frames)
static uint8_t  update_token_chunks_received = 0;
static uint32_t update_session_counter = 0;
static uint8_t  page_buffer[FLASH_PAGE_SIZE];
static uint32_t page_buffer_fill = 0;
static uint32_t current_page_index = 0;
uint8_t  update_in_progress = 0;
uint32_t update_last_activity_tick = 0;
uint8_t  update_failed = 0;

static uint8_t FlushPageBuffer(void) {
    uint32_t page_addr = BACKUP_APP_ADDR + (current_page_index * FLASH_PAGE_SIZE);
    for (uint32_t i = page_buffer_fill; i < FLASH_PAGE_SIZE; i++) {
        page_buffer[i] = 0xFF;
    }
    uint8_t ok = Flash_WriteVerified(page_addr, page_buffer, FLASH_PAGE_SIZE);
    if (ok) {
        CAN_SendPageAck(current_page_index);
        current_page_index++;
        page_buffer_fill = 0;
        HAL_IWDG_Refresh(&hiwdg);
    }
    return ok;
}

// A value that is different in every session, handed to the host on request (OFS_DOWNGRADE_NONCE_REQUEST) and bound into the downgrade token,
// so that a token recorded in one session is useless in another. It is a freshness value, not a secret: it only has to differ from every
// nonce an attacker has seen a token for. This default mixes the state of this boot (tick, SysTick), the unique id of the chip and the
// number of sessions of this boot; a build that wants more should define Boot_SessionEntropy itself with the hardware random generator.
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

static void SendNonceHalf(const uint8_t half[8]) {
    IPC_SendResponse(OFS_DOWNGRADE_NONCE_RESPONSE, half, 8);
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

// Returns 1 when the update was accepted (size and hardware ID valid, backup
// slot erased, ready to receive), 0 when the request was refused - a refused
// request must not pull the board out of its normal start-up.
uint8_t HandleStartUpdate(uint8_t *data) {
    update_total_size = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16)
                       | ((uint32_t)data[2] << 8) | data[3];
    update_declared_hw_id = ((uint32_t)data[4] << 24) | ((uint32_t)data[5] << 16)
                           | ((uint32_t)data[6] << 8) | data[7];

    if (update_total_size == 0 || update_total_size > APP_MAX_SIZE) {
        CAN_SendStatus(STATUS_ERROR);
        return 0;
    }
    if (update_declared_hw_id != THIS_HARDWARE_ID) {
        CAN_SendVerifyFailReason(VERIFY_FAIL_REASON_HARDWARE_ID);
        return 0;
    }

    // Sector-aligned erase, one sector at a time (see bootloader_flash.c's
    // own header) - "pages_needed" stays the LOGICAL page count every other
    // tier's protocol math uses; the sector translation happens inside
    // Flash_ErasePages itself.
    uint32_t pages_needed = (update_total_size + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE;
    CAN_SendStatus(STATUS_ERASING);
    if (!Flash_ErasePages(BACKUP_APP_ADDR, pages_needed)) {
        CAN_SendStatus(STATUS_ERROR);
        update_failed = 1;
        update_in_progress = 0;
        return 0;
    }

    memset(page_buffer, 0xFF, sizeof(page_buffer));
    update_bytes_received = 0;
    update_running_crc = 0xFFFFFFFFUL;
    update_signature_chunks_received = 0;
    update_header_chunks_received = 0;
    update_token_chunks_received = 0;
    memset(update_header, 0, sizeof(update_header));
    memset(update_signature, 0, sizeof(update_signature));
    memset(update_token, 0, sizeof(update_token));
    page_buffer_fill = 0;
    current_page_index = 0;
    update_in_progress = 1;
    update_failed = 0;
    update_last_activity_tick = HAL_GetTick();
    MakeSessionNonce(update_nonce);
    CAN_SendStatus(STATUS_RECEIVING);
    return 1;
}

// The old flow let anybody on the bus switch the anti-rollback off with four constant bytes. That frame is now ignored: a downgrade needs the
// token of OFS_DOWNGRADE_TOKEN, an Ed25519 signature, under the update key, of this session's nonce, the installed and the new version and the digest of the
// image (update_verify.h). Kept as a function so the dispatch of the main loop stays as it was.
void HandleAuthorizeDowngrade(uint8_t *data) {
    (void)data;
}

// see mcu_stm32g474/boot/bootloader_protocol.c's own
// comment for the full design.
void HandleConfirmHealthy(void) {
    FirmwareMetadata_t meta;
    if (!Metadata_Read(&meta) || meta.magic != METADATA_MAGIC_VALID) {
        return;
    }
    if (meta.boot_attempts == 0) {
        return;
    }
    meta.boot_attempts = 0;
    Metadata_EraseAndWrite(&meta);
}

// Backup readback over the mailbox: CM4 relays a BACKUP_READ_REQUEST
// command in, this core streams the main slot back as a sequence of
// IPC_SendResponse(OFS_BACKUP_READ_RESPONSE, ...) frames, pausing after
// each logical page to wait for CM4 to post a BACKUP_READ_PAGE_ACK command
// (the SAME pacing every other tier's own readback uses, just carried by
// the mailbox's .cmd slot instead of a bus frame). update_in_progress is
// checked so a readback never interleaves with an active update - same
// rule every other tier's HandleReadbackStart enforces.
static uint8_t WaitForReadbackPageAck(uint32_t page_index) {
    uint32_t wait_start = HAL_GetTick();
    while ((HAL_GetTick() - wait_start) < 3000) {
        HAL_IWDG_Refresh(&hiwdg);
        if (HAL_HSEM_IsSemTaken(IPC_HSEM_CMD_ID)) {
            uint8_t frame_type = IPC_MAILBOX->cmd.frame_type;
            uint8_t dlc = IPC_MAILBOX->cmd.dlc;
            uint32_t acked = ((uint32_t)IPC_MAILBOX->cmd.payload[0] << 24) | ((uint32_t)IPC_MAILBOX->cmd.payload[1] << 16)
                            | ((uint32_t)IPC_MAILBOX->cmd.payload[2] << 8) | IPC_MAILBOX->cmd.payload[3];
            // Always release once read, even when this isn't the ACK we're
            // waiting for - leaving the semaphore held here (the original
            // bug) makes HAL_HSEM_IsSemTaken keep reporting the same stale
            // frame on every loop iteration, so this function just spins
            // until the 3s timeout and aborts the whole readback instead of
            // discarding the unrelated frame and continuing to wait.
            HAL_HSEM_Release(IPC_HSEM_CMD_ID, 0);
            if (frame_type == OFS_BACKUP_READ_PAGE_ACK && dlc == 4) {
                if (acked == page_index) return 1;
                return 0; // desynced ack for the wrong page - abandon rather than guess
            }
            // Not the ACK we're waiting for - discard and keep waiting.
        }
    }
    return 0;
}

void HandleReadbackStart(void) {
    if (update_in_progress) return;

    FirmwareMetadata_t meta;
    uint32_t total_size = 0;
    // Same guard ApplicationIsValid() applies before trusting meta.size -
    // Metadata_Read() only checks `magic`, so a torn-write metadata page
    // could otherwise leave state==META_STATE_APP_VALID with hardware_id/
    // size still reading as erased flash, and the page loop below would
    // then read past the end of real flash. Reachable via the CM5 mailbox
    // relay, unauthenticated.
    if (Metadata_Read(&meta) && meta.state == META_STATE_APP_VALID &&
        meta.hardware_id == THIS_HARDWARE_ID &&
        meta.size > 0 && meta.size <= APP_MAX_SIZE) {
        total_size = meta.size;
    }

    uint8_t d[4] = {
        (uint8_t)(total_size >> 24), (uint8_t)(total_size >> 16),
        (uint8_t)(total_size >> 8), (uint8_t)(total_size)
    };
    IPC_SendResponse(OFS_BACKUP_READ_RESPONSE, d, 4);
    if (total_size == 0) return;

    uint32_t num_pages = (total_size + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE;
    uint32_t offset = 0;
    for (uint32_t p = 0; p < num_pages; p++) {
        uint32_t page_len = FLASH_PAGE_SIZE;
        if (offset + page_len > total_size) page_len = total_size - offset;

        for (uint32_t i = 0; i < page_len; i += 8) {
            uint8_t chunk_len = (uint8_t)((page_len - i < 8) ? (page_len - i) : 8);
            uint8_t d2[8] = {0};
            memcpy(d2, (const void*)(MAIN_APP_ADDR + offset + i), chunk_len);
            IPC_SendResponse(OFS_BACKUP_READ_RESPONSE, d2, chunk_len);
        }
        offset += page_len;

        if (!WaitForReadbackPageAck(p)) return;
    }
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

void HandleData(uint8_t *data, uint32_t dlc) {
    if (!update_in_progress || update_failed) return;
    if (dlc > 8) return;
    if (update_bytes_received < update_total_size) {
        update_last_activity_tick = HAL_GetTick();
    }

    uint32_t i;
    for (i = 0; i < dlc; i++) {
        if (update_bytes_received >= update_total_size) break;
        page_buffer[page_buffer_fill++] = data[i];
        update_bytes_received++;

        if (page_buffer_fill == FLASH_PAGE_SIZE) {
            if (!FlushPageBuffer()) {
                update_running_crc = CRC32_Update(update_running_crc, data, i + 1);
                CAN_SendStatus(STATUS_ERROR);
                update_in_progress = 0;
                update_failed = 1;
                return;
            }
        }
    }
    update_running_crc = CRC32_Update(update_running_crc, data, i);
}

static int UpdateSignatureUnderTheKey(const uint8_t *msg, uint32_t len, const uint8_t signature[IMAGE_SIGNATURE_SIZE], void *context) {
    (void)context;
    return UpdateSignature_Verify(msg, len, signature, UPDATE_PUBLIC_KEY);
}

// What the host is told when the decision of update_verify.h is not OK.
static uint8_t VerifyFailReasonOf(UpdateVerdict_t verdict) {
    switch (verdict) {
        case UPDATE_VERDICT_INCOMPLETE:   return VERIFY_FAIL_REASON_INCOMPLETE;
        case UPDATE_VERDICT_CRC32:        return VERIFY_FAIL_REASON_CRC32;
        case UPDATE_VERDICT_SIGNATURE:    return VERIFY_FAIL_REASON_SIGNATURE;
        case UPDATE_VERDICT_HARDWARE_ID:  return VERIFY_FAIL_REASON_HARDWARE_ID;
        case UPDATE_VERDICT_ROLLBACK:     return VERIFY_FAIL_REASON_ROLLBACK;
        default:                          return VERIFY_FAIL_REASON_HEADER;
    }
}

void HandleEndUpdate(uint8_t *data) {
    if (!update_in_progress || update_failed) return;
    update_last_activity_tick = HAL_GetTick();

    if (page_buffer_fill > 0) {
        if (!FlushPageBuffer()) {
            CAN_SendStatus(STATUS_ERROR);
            update_in_progress = 0;
            update_failed = 1;
            return;
        }
    }

    uint32_t expected_crc = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16)
                           | ((uint32_t)data[2] << 8) | data[3];
    uint16_t version_major = ((uint16_t)data[4] << 8) | data[5];
    uint16_t version_minor = ((uint16_t)data[6] << 8) | data[7];
    uint32_t actual_crc = CRC32_Finalize(update_running_crc);

    CAN_SendStatus(STATUS_VERIFYING);

    // The signature covers the digest of the 64-byte header and then the image, so the version, the hardware id, the size and the vectors
    // cannot be changed without breaking it. It is only worth a pass over the image when everything arrived (otherwise the verdict is
    // "incomplete").
    uint8_t actual_digest[IMAGE_DIGEST_SIZE];
    memset(actual_digest, 0, sizeof(actual_digest));
    if (update_bytes_received == update_total_size && update_signature_chunks_received == UPDATE_SIGNATURE_CHUNKS &&
        update_header_chunks_received == UPDATE_HEADER_CHUNKS) {
        sha256_prefixed_flash_region(update_header, IMAGE_HEADER_SIZE, BACKUP_APP_ADDR, update_total_size, actual_digest);
    }

    UpdateVerifyInput_t input;
    memset(&input, 0, sizeof(input));
    input.bytes_received = update_bytes_received;
    input.total_size = update_total_size;
    input.signature_chunks = update_signature_chunks_received;
    input.header_chunks = update_header_chunks_received;
    input.token_chunks = update_token_chunks_received;
    input.declared_crc = expected_crc;
    input.actual_crc = actual_crc;
    input.declared_major = version_major;
    input.declared_minor = version_minor;
    input.header = update_header;
    input.signature = update_signature;
    input.actual_digest = actual_digest;
    input.token = update_token;
    input.nonce = update_nonce;
    input.expect.hw_id = THIS_HARDWARE_ID;
    input.expect.link_address = MAIN_APP_ADDR;
    input.expect.first_word = *(volatile const uint32_t*)BACKUP_APP_ADDR;
    input.expect.second_word = *(volatile const uint32_t*)(BACKUP_APP_ADDR + 4UL);
    input.expect.ram_lo = D1_AXISRAM_BASE + 0x400UL;       // the same range StackPointerInValidRAM() accepts: this core's AXI SRAM
    input.expect.ram_hi = D1_AXISRAM_BASE + 0x80000UL;
    input.verify = UpdateSignatureUnderTheKey;

    FirmwareMetadata_t current_meta;
    if (Metadata_Read(&current_meta) && current_meta.state == META_STATE_APP_VALID) {
        input.have_installed = 1;
        input.installed.major = (uint16_t)current_meta.version_major;
        input.installed.minor = (uint16_t)current_meta.version_minor;
        input.installed.patch = METADATA_PATCH(current_meta);
    }

    ImageHeader_t header;
    UpdateVerdict_t verdict = UpdateVerify_Decide(&input, &header);
    if (verdict != UPDATE_VERDICT_OK) {
        CAN_SendVerifyFailReason(VerifyFailReasonOf(verdict));
        update_in_progress = 0;
        update_failed = 1;
        return;
    }

    // What the boot-time check of ApplicationIsValid() compares with is the local-integrity HMAC of the image as it sits in the main
    // slot, without the header (the header is never stored), so that is what the metadata keeps.
    uint8_t image_hmac[32];
    hmac_sha256_flash_region(HMAC_KEY, 32, BACKUP_APP_ADDR, update_total_size, image_hmac);

    FirmwareMetadata_t pending = {0};
    pending.magic = METADATA_MAGIC_VALID;
    pending.state = META_STATE_COPY_PENDING;
    pending.hardware_id = update_declared_hw_id;
    pending.version_major = header.version.major;
    pending.version_minor = header.version.minor;
    pending.version_patch = header.version.patch;
    pending.size = update_total_size;
    pending.crc32 = actual_crc;
    memcpy(pending.hmac, image_hmac, 32);
    if (!Metadata_EraseAndWrite(&pending)) {
        CAN_SendStatus(STATUS_ERROR);
        update_in_progress = 0;
        update_failed = 1;
        return;
    }

    CAN_SendStatus(STATUS_COPYING);
    if (!Flash_CopyRegion(MAIN_APP_ADDR, BACKUP_APP_ADDR, update_total_size)) {
        CAN_SendStatus(STATUS_ERROR);
        update_in_progress = 0;
        update_failed = 1;
        return;
    }

    FirmwareMetadata_t done = pending;
    done.state = META_STATE_APP_VALID;
    if (!Metadata_EraseAndWrite(&done)) {
        CAN_SendStatus(STATUS_ERROR);
        update_in_progress = 0;
        update_failed = 1;
        return;
    }

    CAN_SendStatus(STATUS_VERIFY_OK);
    HAL_Delay(600); // let the status frame actually get out before resetting
    NVIC_SystemReset();
}
