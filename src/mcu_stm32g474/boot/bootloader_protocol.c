// =============================================================================
// HYDRA-UMC Robot Controller Board Bootloader - FDCAN protocol handlers,
// application validation, and jump
// Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
// GPL-3.0-or-later - see repo root LICENSE
//
// Ported from URTC's own proven bootloader_protocol.c. Two real differences
// from that file, everything else is the same state machine:
//   1. bxCAN (CAN_HandleTypeDef/CAN_TxHeaderTypeDef/...) -> FDCAN
//      (FDCAN_HandleTypeDef/FDCAN_TxHeaderTypeDef/...) - different HAL API,
//      same "classic" 11-bit-ID/0-8-byte-DLC frame semantics, so every
//      protocol-level byte layout below is unchanged from URTC's own.
//   2. Every ID is g_slot_base + OFS_xxx (docs/architecture.md section 4)
//      instead of a fixed CAN_ID_xxx - this board only exists on a shared,
//      slot-addressed bus, URTC's own board never did.
// No OLED here (this board has none) - URTC's own progress-display calls
// are simply omitted, not replaced with a no-op stub.
// =============================================================================
#include "stm32g4xx_hal.h"
#include <string.h>
#include "bootloader_common.h"
#include "bootloader_protocol.h"
#include "bootloader_crypto.h"
#include "bootloader_flash.h"
#include "dlc_to_fdcan_data_length.h"
#include "update_verify.h"

// Classic-frame DLC (0-8) <-> FDCAN HAL's DataLength encoding: the HAL's
// FDCAN_DLC_BYTES_n constants are the plain DLC code (n for n = 0..8), and
// the HAL itself shifts the field into the message RAM word when it sends
// and shifts it back down when it receives. Nothing here may shift it again.
#define DLC_TO_HAL(dlc)   DlcToFdcanDataLength((uint8_t)(dlc))
#define HAL_TO_DLC(hdlc)  FdcanDataLengthToDlc((uint32_t)(hdlc))

static uint32_t g_slot_base = 0;

void Protocol_Init(uint32_t slot_base_id) {
    g_slot_base = slot_base_id;
}

static uint8_t CAN_WaitForFreeMailbox(void) {
    // Same 50ms budget as URTC's own bootloader (50 attempts x 1ms), well
    // inside the 1s IWDG window (reload 999, prescaler 32, LSI 32 kHz).
    for (uint8_t attempt = 0; attempt < 50; attempt++) {
        if (HAL_FDCAN_GetTxFifoFreeLevel(&hfdcan1) > 0) return 1;
        HAL_IWDG_Refresh(&hiwdg);
        HAL_Delay(1);
    }
    return HAL_FDCAN_GetTxFifoFreeLevel(&hfdcan1) > 0;
}

static void FDCAN_Send(uint32_t id, const uint8_t *data, uint32_t dlc) {
    if (!CAN_WaitForFreeMailbox()) return; // no mailbox freed up in time - drop rather than block indefinitely
    FDCAN_TxHeaderTypeDef txHeader;
    txHeader.Identifier = id;
    txHeader.IdType = FDCAN_STANDARD_ID;
    txHeader.TxFrameType = FDCAN_DATA_FRAME;
    txHeader.DataLength = DLC_TO_HAL(dlc);
    txHeader.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
    txHeader.BitRateSwitch = FDCAN_BRS_OFF;
    txHeader.FDFormat = FDCAN_CLASSIC_CAN;
    txHeader.TxEventFifoControl = FDCAN_NO_TX_EVENTS;
    txHeader.MessageMarker = 0;
    uint8_t txData[8] = {0};
    if (dlc > 0) memcpy(txData, data, dlc);
    HAL_FDCAN_AddMessageToTxFifoQ(&hfdcan1, &txHeader, txData);
}

void CAN_SendStatus(uint8_t status) {
    uint8_t d[1] = {status};
    FDCAN_Send(g_slot_base + OFS_STATUS, d, 1);
}

static void CAN_SendVerifyFailReason(uint8_t reason) {
    // Same CAN_ID_STATUS offset as CAN_SendStatus, DLC=2 instead of 1:
    // byte[0] is still STATUS_VERIFY_FAIL so anything reading only byte[0]
    // keeps working, byte[1] adds the specific reason.
    uint8_t d[2] = {STATUS_VERIFY_FAIL, reason};
    FDCAN_Send(g_slot_base + OFS_STATUS, d, 2);
}

static void CAN_SendPageAck(uint32_t page_index) {
    uint8_t d[4] = {
        (uint8_t)(page_index >> 24), (uint8_t)(page_index >> 16),
        (uint8_t)(page_index >> 8), (uint8_t)(page_index)
    };
    FDCAN_Send(g_slot_base + OFS_PAGE_ACK, d, 4);
}

void CAN_SendHeartbeat(uint8_t status, uint8_t progress_percent) {
    if (HAL_FDCAN_GetTxFifoFreeLevel(&hfdcan1) == 0) return; // best-effort, never blocks - heartbeat missing one beat is harmless
    uint8_t d[2] = {status, progress_percent};
    FDCAN_Send(g_slot_base + OFS_HEARTBEAT, d, 2);
}

// Version query response - byte0=1 marks this as the bootloader answering
// (as opposed to the application, byte0=0) - same dual-answerable
// convention as URTC's own protocol.
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
    FDCAN_Send(g_slot_base + OFS_VERSION_RESPONSE, d, 8);

    uint8_t d2[3] = {BOOTLOADER_VERSION_MAJOR, BOOTLOADER_VERSION_MINOR, BOOTLOADER_VERSION_PATCH};
    FDCAN_Send(g_slot_base + OFS_BOOTLOADER_VERSION, d2, 3);
}

// TEC/REC read directly from the FDCAN peripheral's own Error Counter
// Register (ECR: REC in bits 14:8, TEC in bits 7:0 - RM0440) rather than
// tracked in software, same reasoning as URTC's own ESR-based read on its
// bxCAN peripheral (different register, same fault-confinement counters
// the CAN protocol itself already requires the hardware to maintain).
void HandleErrorCounterQuery(void) {
    uint32_t ecr = hfdcan1.Instance->ECR;
    uint8_t tec = (uint8_t)(ecr & 0xFF);
    uint8_t rec = (uint8_t)((ecr >> 8) & 0x7F);
    uint8_t d[2] = {tec, rec};
    FDCAN_Send(g_slot_base + OFS_ERROR_COUNTERS_RESPONSE, d, 2);
}

// -----------------------------------------------------------------------
// Stack-pointer plausibility check before trusting a "no metadata yet"
// vector table (first-boot adoption) or before actually jumping.
// STM32G474RE: 96KB of contiguous SRAM at 0x20000000 (80KB SRAM1 + 16KB
// SRAM2, RM0440); the further 32KB of CCM SRAM sits elsewhere in the map and
// is not used. This project's own linker scripts treat the 96KB as one flat
// RAM region (see ../STM32G474RETx_APP.ld) - so unlike URTC's F303 (two
// disjoint regions, SRAM + separate CCM), only one range needs checking.
// -----------------------------------------------------------------------
static uint8_t StackPointerInValidRAM(uint32_t sp) {
    return (sp >= SRAM_BASE + 0x100UL && sp <= SRAM_BASE + 0x18000UL);
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
    HAL_RCC_DeInit();
    HAL_DeInit();
    // Same safe-state pattern URTC's own bootloader uses before a jump -
    // 0xEBFFFFFF/0xFFFFFFFF preserve PA13/PA14 (SWD), force every other
    // GPIOA/GPIOB pin to a disconnected analog-input state.
    GPIOA->MODER = 0xEBFFFFFF;
    GPIOB->MODER = 0xFFFFFFFF;
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
// Update flow - all writes during an update go to the BACKUP slot only.
// The main slot is never touched until backup has fully passed size, CRC32,
// HMAC-SHA256, and HardwareID verification. Identical state machine to
// URTC's own - see that repo's bootloader_protocol.c for the original,
// more heavily-commented version this was ported from.
// -----------------------------------------------------------------------
uint32_t update_total_size = 0;
static uint32_t update_declared_hw_id = 0;
uint32_t update_bytes_received = 0;
static uint32_t update_running_crc = 0xFFFFFFFFUL;
static uint8_t  update_expected_hmac[32];
static uint8_t  update_hmac_chunks_received = 0;
static uint8_t  update_header[IMAGE_HEADER_SIZE];          // the signed header (eight OFS_IMAGE_HEADER frames)
static uint8_t  update_header_chunks_received = 0;
static uint8_t  update_nonce[IMAGE_NONCE_SIZE];            // announced to the host in this session; a downgrade token is bound to it
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

static void SendNonceHalf(const uint8_t half[8]) {
    FDCAN_Send(g_slot_base + OFS_DOWNGRADE_NONCE_RESPONSE, half, 8);
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

// The old flow let anybody on the bus switch the anti-rollback off with four constant bytes. That frame is now ignored: a downgrade
// needs the token of OFS_DOWNGRADE_TOKEN, an HMAC under the update key of this session's nonce, the installed and the new version and
// the tag of the image (update_verify.h). Kept as a function so the dispatch of the main loop stays as it was.
void HandleAuthorizeDowngrade(uint8_t *data) {
    (void)data;
}

// resets FirmwareMetadata_t's own boot_attempts back to 0 -
// see boot_decision.h's own header comment for the full design and why
// this is sent BY an external CAN master, never by the application itself.
// A no-op (never an error) when there is no valid metadata to update yet -
// same honest "nothing to do" precedent HandleReadbackStart's own guards
// already use, not a real failure worth reporting over CAN.
void HandleConfirmHealthy(void) {
    FirmwareMetadata_t meta;
    if (!Metadata_Read(&meta) || meta.magic != METADATA_MAGIC_VALID) {
        return;
    }
    if (meta.boot_attempts == 0) {
        return; // already 0 - skip the real erase/write cycle
    }
    meta.boot_attempts = 0;
    Metadata_EraseAndWrite(&meta);
}

static uint8_t WaitForReadbackPageAck(uint32_t page_index) {
    uint32_t wait_start = HAL_GetTick();
    while ((HAL_GetTick() - wait_start) < 3000) {
        HAL_IWDG_Refresh(&hiwdg);
        if (HAL_FDCAN_GetRxFifoFillLevel(&hfdcan1, FDCAN_RX_FIFO0) > 0) {
            FDCAN_RxHeaderTypeDef rxH;
            uint8_t rxD[8];
            if (HAL_FDCAN_GetRxMessage(&hfdcan1, FDCAN_RX_FIFO0, &rxH, rxD) == HAL_OK) {
                if (rxH.RxFrameType == FDCAN_DATA_FRAME && rxH.IdType == FDCAN_STANDARD_ID &&
                    rxH.Identifier == g_slot_base + OFS_BACKUP_READ_PAGE_ACK &&
                    HAL_TO_DLC(rxH.DataLength) == 4) {
                    uint32_t acked = ((uint32_t)rxD[0] << 24) | ((uint32_t)rxD[1] << 16)
                                    | ((uint32_t)rxD[2] << 8) | rxD[3];
                    if (acked == page_index) return 1;
                }
            }
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
    // then read past the end of real flash. Reachable by any bus node,
    // unauthenticated.
    if (Metadata_Read(&meta) && meta.state == META_STATE_APP_VALID &&
        meta.hardware_id == THIS_HARDWARE_ID &&
        meta.size > 0 && meta.size <= APP_MAX_SIZE) {
        total_size = meta.size;
    }

    uint8_t d[4] = {
        (uint8_t)(total_size >> 24), (uint8_t)(total_size >> 16),
        (uint8_t)(total_size >> 8), (uint8_t)(total_size)
    };
    FDCAN_Send(g_slot_base + OFS_BACKUP_READ_RESPONSE, d, 4);
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
            FDCAN_Send(g_slot_base + OFS_BACKUP_READ_RESPONSE, d2, chunk_len);
        }
        offset += page_len;

        if (!WaitForReadbackPageAck(p)) return;
    }
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
    input.expect.ram_lo = SRAM_BASE + 0x100UL;       // the same range StackPointerInValidRAM() accepts
    input.expect.ram_hi = SRAM_BASE + 0x18000UL;
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
    HAL_Delay(600); // let the status frame actually get out before resetting
    NVIC_SystemReset();
}
