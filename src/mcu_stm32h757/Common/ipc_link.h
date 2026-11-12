// =============================================================================
// HYDRA-UMC Kinematic Brain - the operations on the IPC mailbox between the two cores, the same for whichever core owns the host link
// Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
// GPL-3.0-or-later - see repo root LICENSE
//
// The mailbox of ipc_mailbox.h has two frames and two semaphore channels: the GATEWAY core (the one that owns SPI1 and FDCAN1) posts a command and waits for the answer; the
// BACKEND core (the other one) takes the command and posts the answer. On the board of today the gateway is the CM4; on the H757 board the owner decided that the CM7 owns SPI1, FDCAN1
// and I2C4, so the gateway is the CM7 and the CM4 becomes the backend. This header holds the four operations once, with the rules the two cores already follow, so that either core can
// be the gateway and the host test can run both sides against each other:
//
//   - Ipc_PostCommand (gateway): waits for the previous command to be consumed; when it is not within the time, it returns IPC_BUSY WITHOUT touching the frame (the single-writer rule:
//     a frame may be written only while the other core cannot be reading it); otherwise it fills the frame (bytes after the length are zero), seals it with a sequence number and a CRC,
//     executes a data barrier and only then takes the semaphore;
//   - Ipc_TakeCommand (backend): when the semaphore is taken, copies the frame, checks the seal and releases the semaphore; a damaged command is reported and dropped;
//   - Ipc_PostResponse (backend): the same as the first, for the answer, with a time-out that drops the answer instead of blocking;
//   - Ipc_WaitResponse (gateway): polls for the answer for a time, copies it, checks the seal and releases the semaphore.
// The hardware (the semaphore, the data barrier, the millisecond tick and the watchdog refresh) is reached through the callbacks of IpcOps. The firmware of both cores still has these
// sequences written out in place; this header is the host-tested form they are meant to move to together with the drivers. Nothing here has run on a board.
// =============================================================================
#ifndef HYDRA_IPC_LINK_H
#define HYDRA_IPC_LINK_H

#include <stdint.h>
#include "ipc_mailbox.h"
#include "platform_profile.h"

// The core that owns the host link is the gateway, the other one the backend: the owner of SPI1 in the platform profile.
#define IPC_GATEWAY_CORE HYDRA_OWNER_SPI1
#define IPC_BACKEND_CORE (HYDRA_OWNER_SPI1 == HYDRA_OWNER_CM7 ? HYDRA_OWNER_CM4 : HYDRA_OWNER_CM7)
_Static_assert(HYDRA_OWNER_SPI1 == HYDRA_OWNER_FDCAN1 && HYDRA_OWNER_FDCAN1 == HYDRA_OWNER_I2C4, "one core owns SPI1, FDCAN1 and I2C4: it is the gateway");

#define IPC_POST_COMMAND_WAIT_MS  50u
#define IPC_WAIT_RESPONSE_MS      200u
#define IPC_POST_RESPONSE_WAIT_MS 100u

typedef enum {
    IPC_OK = 0,
    IPC_NONE = 1,      /* nothing posted */
    IPC_BUSY = 2,      /* the previous frame was not consumed in time: nothing was written */
    IPC_TIMEOUT = 3,   /* no answer in time */
    IPC_DAMAGED = 4    /* a frame whose seal does not match was dropped */
} IpcStatus;

typedef struct {
    uint32_t (*tick_ms)(void *ctx);
    void (*refresh)(void *ctx);                      /* the watchdog refresh, called while waiting */
    int (*sem_taken)(void *ctx, uint32_t channel);
    void (*sem_take)(void *ctx, uint32_t channel);
    void (*sem_release)(void *ctx, uint32_t channel);
    void (*barrier)(void *ctx);                      /* the data barrier before a semaphore is taken to signal */
} IpcOps;

typedef struct {
    uint8_t frame_type;
    uint8_t dlc;
    uint8_t payload[8];
} IpcMessage;

static inline void Ipc_Fill(volatile IpcFrame_t *frame, uint8_t type, const uint8_t *data, uint8_t dlc, uint8_t sequence)
{
    const uint8_t n = dlc > 8u ? 8u : dlc;
    frame->frame_type = type;
    frame->dlc = dlc;
    for (uint32_t i = 0; i < 8u; i++) frame->payload[i] = (i < n && data != 0) ? data[i] : 0u;
    IpcFrame_Seal(frame, sequence);
}

static inline void Ipc_Copy(const volatile IpcFrame_t *frame, IpcMessage *out)
{
    out->frame_type = frame->frame_type;
    out->dlc = frame->dlc;
    for (uint32_t i = 0; i < 8u; i++) out->payload[i] = frame->payload[i];
}

static inline IpcStatus Ipc_Post(const IpcOps *ops, void *ctx, volatile IpcFrame_t *frame, uint32_t channel, uint8_t *sequence, uint8_t type, const uint8_t *data, uint8_t dlc, uint32_t wait_ms)
{
    const uint32_t start = ops->tick_ms(ctx);
    while (ops->sem_taken(ctx, channel)) {
        ops->refresh(ctx);
        if (ops->tick_ms(ctx) - start > wait_ms) return IPC_BUSY;   /* the frame is not touched */
    }
    Ipc_Fill(frame, type, data, dlc, (*sequence)++);
    ops->barrier(ctx);
    ops->sem_take(ctx, channel);
    return IPC_OK;
}

static inline IpcStatus Ipc_Take(const IpcOps *ops, void *ctx, const volatile IpcFrame_t *frame, uint32_t channel, IpcMessage *out)
{
    if (!ops->sem_taken(ctx, channel)) return IPC_NONE;
    Ipc_Copy(frame, out);
    const uint8_t valid = IpcFrame_Valid(frame);
    ops->sem_release(ctx, channel);
    return valid ? IPC_OK : IPC_DAMAGED;
}

static inline IpcStatus Ipc_PostCommand(const IpcOps *ops, void *ctx, IpcMailbox_t *mailbox, uint8_t *sequence, uint8_t type, const uint8_t *data, uint8_t dlc)
{
    return Ipc_Post(ops, ctx, &mailbox->cmd, IPC_HSEM_CMD_ID, sequence, type, data, dlc, IPC_POST_COMMAND_WAIT_MS);
}

static inline IpcStatus Ipc_TakeCommand(const IpcOps *ops, void *ctx, IpcMailbox_t *mailbox, IpcMessage *out)
{
    return Ipc_Take(ops, ctx, &mailbox->cmd, IPC_HSEM_CMD_ID, out);
}

static inline IpcStatus Ipc_PostResponse(const IpcOps *ops, void *ctx, IpcMailbox_t *mailbox, uint8_t *sequence, uint8_t type, const uint8_t *data, uint8_t dlc)
{
    return Ipc_Post(ops, ctx, &mailbox->resp, IPC_HSEM_RESP_ID, sequence, type, data, dlc, IPC_POST_RESPONSE_WAIT_MS);
}

static inline IpcStatus Ipc_WaitResponse(const IpcOps *ops, void *ctx, IpcMailbox_t *mailbox, IpcMessage *out)
{
    const uint32_t start = ops->tick_ms(ctx);
    while (ops->tick_ms(ctx) - start < IPC_WAIT_RESPONSE_MS) {
        ops->refresh(ctx);
        const IpcStatus status = Ipc_Take(ops, ctx, &mailbox->resp, IPC_HSEM_RESP_ID, out);
        if (status != IPC_NONE) return status;
    }
    return IPC_TIMEOUT;
}

#endif // HYDRA_IPC_LINK_H
