// =============================================================================
// HYDRA-UMC Bootloader - boot_update_core_impl.h: the page buffer, the start of an update, the confirmation of a healthy start and the data frames
// Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
// GPL-3.0-or-later - see repo root LICENSE
//
// One copy for the three bootloaders (firmware_base/boot, kept identical in each boot folder by tools/sync_firmware_base.py). It is a text include
// of bootloader_protocol.c, placed after boot_update_glue.h, because it works on the static state of the protocol file (the page buffer, the counters
// of the update). The functions were identical in the three files, checked function by function before they were moved; how a frame leaves
// (CAN_SendStatus, CAN_SendPageAck) is declared in bootloader_protocol.h and stays in each file.
// =============================================================================
#ifndef BOOT_UPDATE_CORE_IMPL_H
#define BOOT_UPDATE_CORE_IMPL_H

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

#endif /* BOOT_UPDATE_CORE_IMPL_H */
