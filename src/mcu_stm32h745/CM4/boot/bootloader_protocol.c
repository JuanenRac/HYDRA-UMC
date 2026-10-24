// =============================================================================
// HYDRA-UMC Kinematic Brain Bootloader (CM4) - self-update handlers,
// application validation/jump, and the 3-way relay (self/CM7/STACK A)
// Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
// GPL-3.0-or-later - see repo root LICENSE
//
// The self-update state machine below (HandleStartUpdate..HandleEndUpdate,
// ApplicationIsValid, JumpToApplication) is the same one every other
// bootloader in this project already implements, ported via the CM7
// version (../../CM7/boot/bootloader_protocol.c) - see that file's own
// comments for the original, more heavily-annotated source this was
// derived from. What ONLY this core has: Relay_ToStackA/Relay_ToCM7 below,
// and CAN_SendStatus/CAN_SendHeartbeat writing into a pending-response
// buffer instead of transmitting directly - see bootloader_common.h's own
// header comment for why this core alone needs all of this.
// =============================================================================
#include "stm32h7xx_hal.h"
#include <string.h>
#include "bootloader_common.h"
#include "bootloader_protocol.h"
#include "bootloader_crypto.h"
#include "bootloader_flash.h"
#include "update_verify.h"
#include "dlc_to_fdcan_data_length.h"
#include "ipc_mailbox.h"

// -----------------------------------------------------------------------
// Pending-response buffer - see bootloader_protocol.h's own comment on
// CAN_SendStatus/CAN_SendHeartbeat. Single-slot (not a queue): the self-
// update handlers below only ever have one outstanding response at a time
// (they're called synchronously from bootloader_main.c's own SPI1 loop,
// one received frame fully handled before the next is read), so a queue
// would add complexity with nothing to actually queue.
// -----------------------------------------------------------------------
static SpiOtaFrame_t g_pending_response;
static uint8_t g_pending_response_dirty = 0;

static void StashResponse(uint8_t frame_type, const uint8_t *data, uint8_t dlc) {
    memset(&g_pending_response, 0, sizeof(g_pending_response));
    g_pending_response.target_tier = SPI_TARGET_SELF;
    g_pending_response.frame_type = frame_type;
    g_pending_response.dlc = dlc;
    if (dlc > 0) memcpy(g_pending_response.payload, data, dlc > 8 ? 8 : dlc);
    g_pending_response_dirty = 1;
}

uint8_t Protocol_TakePendingResponse(SpiOtaFrame_t *out) {
    if (!g_pending_response_dirty) return 0;
    memcpy(out, &g_pending_response, sizeof(*out));
    g_pending_response_dirty = 0;
    return 1;
}

void CAN_SendStatus(uint8_t status) {
    uint8_t d[1] = {status};
    StashResponse(OFS_STATUS, d, 1);
}

static void CAN_SendVerifyFailReason(uint8_t reason) {
    uint8_t d[2] = {STATUS_VERIFY_FAIL, reason};
    StashResponse(OFS_STATUS, d, 2);
}

static void CAN_SendPageAck(uint32_t page_index) {
    uint8_t d[4] = {
        (uint8_t)(page_index >> 24), (uint8_t)(page_index >> 16),
        (uint8_t)(page_index >> 8), (uint8_t)(page_index)
    };
    StashResponse(OFS_PAGE_ACK, d, 4);
}

void CAN_SendHeartbeat(uint8_t status, uint8_t progress_percent) {
    uint8_t d[2] = {status, progress_percent};
    StashResponse(OFS_HEARTBEAT, d, 2);
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
    StashResponse(OFS_VERSION_RESPONSE, d, 8);
    // NOTE: unlike every other tier's HandleVersionQuery, this doesn't
    // ALSO send a separate OFS_BOOTLOADER_VERSION frame right after - the
    // single-slot pending-response buffer above only holds one frame at a
    // time, and the next SPI1 transaction is what drains it. A caller
    // wanting the bootloader version specifically should query it as its
    // own SPI1 transaction rather than expect two responses to one.
}

void HandleErrorCounterQuery(void) {
    uint32_t ecr = hfdcan1.Instance->ECR;
    uint8_t tec = (uint8_t)(ecr & 0xFF);
    uint8_t rec = (uint8_t)((ecr >> 8) & 0x7F);
    uint8_t d[2] = {tec, rec};
    StashResponse(OFS_ERROR_COUNTERS_RESPONSE, d, 2);
}

static uint8_t StackPointerInValidRAM(uint32_t sp) {
    // CM4's own D2-domain SRAM1+2+3: 288KB @ 0x30000000 (see ../STM32H745
    // ZITx_CM4_APP.ld).
    return (sp >= D2_AHBSRAM_BASE + 0x400UL && sp <= D2_AHBSRAM_BASE + 0x48000UL); // 0x30000000, the RAM the app links against - not the 0x10000000 alias
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

    HAL_NVIC_DisableIRQ(FDCAN1_IT0_IRQn);
    HAL_FDCAN_DeInit(&hfdcan1);
    HAL_SPI_DeInit(&hspi1);
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
// Self-update flow - identical state machine to every other bootloader in
// this project.
// -----------------------------------------------------------------------
uint32_t update_total_size = 0;
static uint32_t update_declared_hw_id = 0;
uint32_t update_bytes_received = 0;
static uint32_t update_running_crc = 0xFFFFFFFFUL;
static uint8_t  update_expected_hmac[32];
static uint8_t  update_hmac_chunks_received = 0;
static uint8_t  update_header[IMAGE_HEADER_SIZE];          // the signed header (eight OFS_IMAGE_HEADER frames)
static uint8_t  update_header_chunks_received = 0;
static uint8_t  update_nonce[IMAGE_NONCE_SIZE];            // the nonce of this session; a downgrade token is bound to it
static uint8_t  update_token[IMAGE_TAG_SIZE];              // the optional downgrade token (four OFS_DOWNGRADE_TOKEN frames)
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
    StashResponse(OFS_DOWNGRADE_NONCE_RESPONSE, half, 8);
}

static void MakeSessionNonce(uint8_t nonce[IMAGE_NONCE_SIZE]) {
    uint8_t material[20];
    uint8_t digest[32];
    Boot_SessionEntropy(material);
    update_session_counter++;
    memcpy(material + 16, &update_session_counter, 4);
    hmac_sha256_buffer(HMAC_KEY, 32, material, sizeof(material), digest);
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
    update_hmac_chunks_received = 0;
    update_header_chunks_received = 0;
    update_token_chunks_received = 0;
    memset(update_header, 0, sizeof(update_header));
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
// token of OFS_DOWNGRADE_TOKEN, an HMAC under the update key of this session's nonce, the installed and the new version and the tag of the
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

// Backup readback: simplified relative to every other tier's own version
// (no separate page-ack pacing loop) - the pending-response single-slot
// buffer already provides natural backpressure, since this core can't
// stash a NEW page until the CM5 has clocked out the previous SPI1
// transaction and asked again. Each call sends ONE more chunk; the CM5
// side re-issues OFS_BACKUP_READ_REQUEST per chunk it wants, rather than
// this bootloader streaming the whole thing unprompted the way the CAN-
// bus-based tiers do (SPI1 is master-clocked - this core cannot push data
// the CM5 hasn't asked for by clocking a transaction).
void HandleReadbackStart(void) {
    static uint32_t s_offset = 0;
    if (update_in_progress) return;

    FirmwareMetadata_t meta;
    uint32_t total_size = 0;
    // Same guard ApplicationIsValid() applies before trusting meta.size -
    // Metadata_Read() only checks `magic`, so a torn-write metadata page
    // could otherwise leave state==META_STATE_APP_VALID with hardware_id/
    // size still reading as erased flash, and the memcpy below would then
    // read past the end of real flash. Reachable over SPI1 from the CM5,
    // unauthenticated.
    if (Metadata_Read(&meta) && meta.state == META_STATE_APP_VALID &&
        meta.hardware_id == THIS_HARDWARE_ID &&
        meta.size > 0 && meta.size <= APP_MAX_SIZE) {
        total_size = meta.size;
    }

    if (s_offset >= total_size) {
        s_offset = 0; // wrap - a fresh readback request after a complete pass starts over
        uint8_t d[4] = {
            (uint8_t)(total_size >> 24), (uint8_t)(total_size >> 16),
            (uint8_t)(total_size >> 8), (uint8_t)(total_size)
        };
        StashResponse(OFS_BACKUP_READ_RESPONSE, d, 4);
        return;
    }

    uint8_t chunk_len = (uint8_t)((total_size - s_offset < 8) ? (total_size - s_offset) : 8);
    uint8_t d[8] = {0};
    memcpy(d, (const void*)(MAIN_APP_ADDR + s_offset), chunk_len);
    StashResponse(OFS_BACKUP_READ_RESPONSE, d, chunk_len);
    s_offset += chunk_len;
}

void HandleHmacChunk(uint8_t *data) {
    if (!update_in_progress || update_failed) return;
    if (update_hmac_chunks_received >= 4) return;
    memcpy(&update_expected_hmac[update_hmac_chunks_received * 8], data, 8);
    update_hmac_chunks_received++;
    update_last_activity_tick = HAL_GetTick();
}

// Eight frames of eight bytes, in order, right after START_UPDATE: the 64-byte header the tag was made over (image_header.h).
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

// Four frames of eight bytes, optional: the token that allows one downgrade. Nothing is believed until END_UPDATE checks it.
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

static void UpdateHmacUnderTheKey(const uint8_t *msg, uint32_t len, uint8_t out[32], void *context) {
    (void)context;
    hmac_sha256_buffer(HMAC_KEY, 32, msg, len, out);
}

// What the host is told when the decision of update_verify.h is not OK.
static uint8_t VerifyFailReasonOf(UpdateVerdict_t verdict) {
    switch (verdict) {
        case UPDATE_VERDICT_INCOMPLETE:   return VERIFY_FAIL_REASON_INCOMPLETE;
        case UPDATE_VERDICT_CRC32:        return VERIFY_FAIL_REASON_CRC32;
        case UPDATE_VERDICT_HMAC:         return VERIFY_FAIL_REASON_HMAC;
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

    // The tag covers the 64-byte header and then the image, so the version, the hardware id, the size and the vectors cannot be changed
    // without breaking it. It is only worth a pass over the image when everything arrived (otherwise the verdict is "incomplete").
    uint8_t actual_tag[IMAGE_TAG_SIZE];
    memset(actual_tag, 0, sizeof(actual_tag));
    if (update_bytes_received == update_total_size && update_hmac_chunks_received == UPDATE_HMAC_CHUNKS &&
        update_header_chunks_received == UPDATE_HEADER_CHUNKS) {
        hmac_sha256_prefixed_flash_region(HMAC_KEY, 32, update_header, IMAGE_HEADER_SIZE, BACKUP_APP_ADDR, update_total_size, actual_tag);
    }

    UpdateVerifyInput_t input;
    memset(&input, 0, sizeof(input));
    input.bytes_received = update_bytes_received;
    input.total_size = update_total_size;
    input.hmac_chunks = update_hmac_chunks_received;
    input.header_chunks = update_header_chunks_received;
    input.token_chunks = update_token_chunks_received;
    input.declared_crc = expected_crc;
    input.actual_crc = actual_crc;
    input.declared_major = version_major;
    input.declared_minor = version_minor;
    input.header = update_header;
    input.expected_tag = update_expected_hmac;
    input.actual_tag = actual_tag;
    input.token = update_token;
    input.nonce = update_nonce;
    input.expect.hw_id = THIS_HARDWARE_ID;
    input.expect.link_address = MAIN_APP_ADDR;
    input.expect.first_word = *(volatile const uint32_t*)BACKUP_APP_ADDR;
    input.expect.second_word = *(volatile const uint32_t*)(BACKUP_APP_ADDR + 4UL);
    input.expect.ram_lo = D2_AHBSRAM_BASE + 0x400UL;       // the same range StackPointerInValidRAM() accepts: the D2-domain SRAM this core's application links against
    input.expect.ram_hi = D2_AHBSRAM_BASE + 0x48000UL;
    input.hmac = UpdateHmacUnderTheKey;

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

    // What the boot-time check of ApplicationIsValid() compares with is the HMAC of the image as it sits in the main slot, without the
    // header (the header is never stored), so that is what the metadata keeps.
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
    // Unlike every other tier, this core does NOT HAL_Delay()+
    // NVIC_SystemReset() immediately - the success status still needs to
    // go out over SPI1 on the NEXT transaction (this function only
    // stashed it above), and resetting now would lose it. bootloader_
    // main.c's own SPI1 loop checks update_in_progress/STATUS_VERIFY_OK
    // and performs the actual delayed reset AFTER that transaction
    // completes - see it for the real reset trigger.
}

// -----------------------------------------------------------------------
// Relay to a Robot Controller Board (or, via ITS own RELAY_SEND/RELAY_RECV
// tunnel, to a URTC Tool Head / Advanced Expansion Board one or two hops
// further - opaque to this function, see bootloader_common.h's own header)
// -----------------------------------------------------------------------
void Relay_ToStackA(uint8_t slot, const SpiOtaFrame_t *in, SpiOtaFrame_t *out) {
    // BOARD_ID[2:0] is a 3-bit local DIP switch (docs/PINOUT_STM32G474_
    // ROBOT_CONTROLLER.TXT section 1c) - only slots 0-7 can ever exist on
    // the bus. slot >= 16 would push target_id past the 11-bit standard
    // CAN ID range (undefined HAL behavior); this rejects anything past
    // the actually-addressable range instead of only the ones that would
    // overflow the ID width, matching how the rest of this codebase
    // validates its other externally-supplied inputs explicitly.
    // in->frame_type is also externally supplied (the CM5's own SPI1 frame,
    // byte 2 - no CRC on the raw wire frame, see bootloader_main.c's own
    // WireToFrame) and is used below as a raw ADDEND onto this slot's own
    // base ID. Without this check, frame_type >= STACKA_SLOT_WINDOW (0x20)
    // pushes target_id PAST this slot's own 32-ID window and into the NEXT
    // slot's (or a further slot's) command space - e.g. slot=0 with a
    // corrupted frame_type=0x21 computes the exact same ID as slot=1's own
    // +0x01 (START_UPDATE), silently addressing a DIFFERENT robot than the
    // one the CM5 actually asked for. A bit-flipped or malformed SPI1 frame
    // must not be able to steer a command onto an unrelated robot's own
    // command ID - reject out-of-window offsets the same way slot>7 is
    // already rejected above, rather than only bounding the slot number.
    if (slot > 7 || in->frame_type >= STACKA_SLOT_WINDOW) {
        memset(out, 0, sizeof(*out));
        out->target_tier = SPI_TARGET_STACKA;
        out->target_slot = slot;
        out->frame_type = OFS_STATUS;
        out->dlc = 1;
        out->payload[0] = STATUS_ERROR;
        return;
    }
    uint32_t slot_base = CAN_ID_STACKA_BASE + (uint32_t)slot * STACKA_SLOT_WINDOW;
    uint32_t target_id = slot_base + in->frame_type;
    // One emitter per identifier (tools/can_ids.py): the identifiers the controller board answers on, and the ones the bootloader itself transmits, are never
    // transmitted from here, whatever the CM5 asked for. A POLL_STATUS frame is the CM5 asking to be handed the next frame of the board: it puts nothing on the bus.
    uint8_t listen_only = (in->frame_type == OFS_POLL_STATUS);
    uint8_t transmitted_by_the_board = (in->frame_type == OFS_AXIS_STATUS_RESPONSE || in->frame_type == OFS_RELAY_RECV_RESPONSE ||
                                        in->frame_type == OFS_PAGE_ACK || in->frame_type == OFS_STATUS || in->frame_type == OFS_HEARTBEAT ||
                                        in->frame_type == OFS_VERSION_RESPONSE || in->frame_type == OFS_BOOTLOADER_VERSION ||
                                        in->frame_type == OFS_ERROR_COUNTERS_RESPONSE || in->frame_type == OFS_BACKUP_READ_RESPONSE ||
                                        in->frame_type == OFS_DOWNGRADE_NONCE_RESPONSE);
    if (transmitted_by_the_board) {
        memset(out, 0, sizeof(*out));
        out->target_tier = SPI_TARGET_STACKA;
        out->target_slot = slot;
        out->frame_type = OFS_STATUS;
        out->dlc = 1;
        out->payload[0] = STATUS_ERROR;
        return;
    }

    FDCAN_TxHeaderTypeDef txHeader;
    txHeader.Identifier = target_id;
    txHeader.IdType = FDCAN_STANDARD_ID;
    txHeader.TxFrameType = FDCAN_DATA_FRAME;
    txHeader.DataLength = DlcToFdcanDataLength(in->dlc); // the HAL takes the plain DLC code and shifts it itself
    txHeader.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
    txHeader.BitRateSwitch = FDCAN_BRS_OFF;
    txHeader.FDFormat = FDCAN_CLASSIC_CAN;
    txHeader.TxEventFifoControl = FDCAN_NO_TX_EVENTS;
    txHeader.MessageMarker = 0;
    uint8_t txData[8] = {0};
    memcpy(txData, (const void*)in->payload, in->dlc > 8 ? 8 : in->dlc);
    if (!listen_only && HAL_FDCAN_GetTxFifoFreeLevel(&hfdcan1) > 0) {
        HAL_FDCAN_AddMessageToTxFifoQ(&hfdcan1, &txHeader, txData);
    }

    memset(out, 0, sizeof(*out));
    out->target_tier = SPI_TARGET_STACKA;
    out->target_slot = slot;
    out->frame_type = OFS_STATUS;
    out->dlc = 1;
    out->payload[0] = STATUS_ERROR; // default if nothing comes back in time - "the slot didn't answer" IS meaningful information to relay to the CM5

    uint32_t wait_start = HAL_GetTick();
    while ((HAL_GetTick() - wait_start) < 200) { // bounded wait, well inside this core's own IWDG window
        HAL_IWDG_Refresh(&hiwdg);
        if (HAL_FDCAN_GetRxFifoFillLevel(&hfdcan1, FDCAN_RX_FIFO0) > 0) {
            FDCAN_RxHeaderTypeDef rxHeader;
            uint8_t rxData[8];
            if (HAL_FDCAN_GetRxMessage(&hfdcan1, FDCAN_RX_FIFO0, &rxHeader, rxData) == HAL_OK) {
                if (rxHeader.RxFrameType == FDCAN_DATA_FRAME && rxHeader.IdType == FDCAN_STANDARD_ID &&
                    rxHeader.Identifier >= slot_base && rxHeader.Identifier < slot_base + STACKA_SLOT_WINDOW) {
                    out->frame_type = (uint8_t)(rxHeader.Identifier - slot_base);
                    out->dlc = (uint8_t)FdcanDataLengthToDlc(rxHeader.DataLength); // the HAL already shifted the field down
                    memcpy(out->payload, rxData, 8);
                    return;
                }
                // frame from a DIFFERENT slot (another board's own
                // heartbeat, say) - not what this relay is waiting for,
                // drop and keep waiting rather than misattributing it.
            }
        }
    }
}

// -----------------------------------------------------------------------
// Relay to CM7 over the IPC mailbox (../../Common/ipc_mailbox.h)
// -----------------------------------------------------------------------
void Relay_ToCM7(const SpiOtaFrame_t *in, SpiOtaFrame_t *out) {
    uint32_t wait_start = HAL_GetTick();
    while (HAL_HSEM_IsSemTaken(IPC_HSEM_CMD_ID)) {
        // previous command not yet consumed by CM7 - give it a little room
        // before giving up. Unlike the original version of this loop,
        // giving up here means returning an honest error WITHOUT touching
        // .cmd - ipc_mailbox.h's own single-writer invariant only holds if
        // CM4 never writes .cmd while CM7 might still be mid-read of the
        // previous frame, and this struct has no sequence number CM7 could
        // use to detect a torn read. A possibly-corrupted command reaching
        // CM7's protocol handler is worse than an honest "CM7 busy" status
        // relayed back to the CM5.
        HAL_IWDG_Refresh(&hiwdg);
        if (HAL_GetTick() - wait_start > 50) {
            memset(out, 0, sizeof(*out));
            out->target_tier = SPI_TARGET_CM7;
            out->frame_type = OFS_STATUS;
            out->dlc = 1;
            out->payload[0] = STATUS_ERROR;
            return;
        }
    }
    IPC_MAILBOX->cmd.frame_type = in->frame_type;
    IPC_MAILBOX->cmd.dlc = in->dlc;
    memset((void*)IPC_MAILBOX->cmd.payload, 0, 8);
    memcpy((void*)IPC_MAILBOX->cmd.payload, (const void*)in->payload, in->dlc > 8 ? 8 : in->dlc);
    // SRAM4 writes above are Normal (bufferable) memory; the HSEM take just
    // below is a peripheral (Device) write - the two bus types have no
    // ordering guarantee relative to each other without an explicit
    // barrier. Without this __DSB(), the store buffer could let the HSEM
    // take reach CM7 before the .cmd field writes have actually landed in
    // SRAM4, so CM7 could observe the semaphore taken and read a stale or
    // torn command frame. Same "flush shared data before signaling"
    // pattern ST's own dual-core HSEM application notes require for this
    // exact producer/consumer shape.
    __DSB();
    HAL_HSEM_FastTake(IPC_HSEM_CMD_ID);

    memset(out, 0, sizeof(*out));
    out->target_tier = SPI_TARGET_CM7;
    out->frame_type = OFS_STATUS;
    out->dlc = 1;
    out->payload[0] = STATUS_ERROR;

    wait_start = HAL_GetTick();
    while ((HAL_GetTick() - wait_start) < 200) {
        HAL_IWDG_Refresh(&hiwdg);
        if (HAL_HSEM_IsSemTaken(IPC_HSEM_RESP_ID)) {
            out->frame_type = IPC_MAILBOX->resp.frame_type;
            out->dlc = IPC_MAILBOX->resp.dlc;
            memcpy(out->payload, (const void*)IPC_MAILBOX->resp.payload, 8);
            HAL_HSEM_Release(IPC_HSEM_RESP_ID, 0);
            return;
        }
    }
}
