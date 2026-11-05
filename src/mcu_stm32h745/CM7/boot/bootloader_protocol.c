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
#include "bootloader_trust.h"
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
    static uint8_t ipc_resp_seq;
    IpcFrame_Seal(&IPC_MAILBOX->resp, ipc_resp_seq++);
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
        adopted.counter = Trust_MinCounter();
        sha256_prefixed_flash_region(NULL, 0, MAIN_APP_ADDR, APP_MAX_SIZE, adopted.sha256);
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

    uint8_t actual_digest[32];
    sha256_prefixed_flash_region(NULL, 0, MAIN_APP_ADDR, meta.size, actual_digest);
    if (!digest_constant_time_compare(actual_digest, meta.sha256, 32)) return 0;

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

static void SendNonceHalf(const uint8_t half[8]) {
    IPC_SendResponse(OFS_DOWNGRADE_NONCE_RESPONSE, half, 8);
}

#include "boot_update_glue.h" // the update flow that is the same in the three bootloaders

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
            const uint8_t ipc_valid = IpcFrame_Valid(&IPC_MAILBOX->cmd);
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
            if (ipc_valid && frame_type == OFS_BACKUP_READ_PAGE_ACK && dlc == 4) {
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

    UpdateVerify2Input_t input;
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
    const TrustKeys_t *trust = Trust_Active();
    input.key_count = trust->count;
    input.key_ids = (const uint8_t (*)[IMAGE_KEY_ID_SIZE])trust->key_id;
    input.keys = (const uint8_t (*)[32])trust->keys;
    input.verify = UpdateSignatureUnderTheKey;

    FirmwareMetadata_t current_meta;
    uint8_t have_meta = Metadata_Read(&current_meta);
    input.counter_floor = Trust_MinCounter();
    if (have_meta && current_meta.counter > input.counter_floor) input.counter_floor = current_meta.counter;
    if (have_meta && current_meta.state == META_STATE_APP_VALID) {
        input.have_installed = 1;
        input.installed.major = (uint16_t)current_meta.version_major;
        input.installed.minor = (uint16_t)current_meta.version_minor;
        input.installed.patch = METADATA_PATCH(current_meta);
    }

    ImageHeader2_t header;
    UpdateVerdict_t verdict = UpdateVerify_Decide2(&input, &header);
    if (verdict != UPDATE_VERDICT_OK) {
        CAN_SendVerifyFailReason(VerifyFailReasonOf(verdict));
        update_in_progress = 0;
        update_failed = 1;
        return;
    }

    // What the boot-time check of ApplicationIsValid() compares with is the SHA-256 of the image as it sits in the main slot, without the
    // header (the header is never stored), so that is what the metadata keeps; it needs no key, the signature was checked above.
    uint8_t image_digest[32];
    sha256_prefixed_flash_region(NULL, 0, BACKUP_APP_ADDR, update_total_size, image_digest);

    FirmwareMetadata_t pending = {0};
    pending.magic = METADATA_MAGIC_VALID;
    pending.state = META_STATE_COPY_PENDING;
    pending.hardware_id = update_declared_hw_id;
    pending.version_major = header.base.version.major;
    pending.version_minor = header.base.version.minor;
    pending.version_patch = header.base.version.patch;
    pending.size = update_total_size;
    pending.crc32 = actual_crc;
    memcpy(pending.sha256, image_digest, 32);
    pending.counter = header.rollback_counter;   // the new floor: nothing signed with a lower counter installs again
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
