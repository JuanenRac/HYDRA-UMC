// =============================================================================
// HYDRA-UMC Kinematic Brain - the operations on the IPC mailbox between the two cores, the same for whichever core owns the host link
// Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
// GPL-3.0-or-later - see repo root LICENSE
//
// The mailbox of ipc_mailbox.h has two frames and four semaphores. The GATEWAY core (the one that owns SPI1 and FDCAN1) posts a command and waits for the answer; the BACKEND core
// (the other one) takes the command and posts the answer. On the board of today the gateway is the CM4; on the H757 board the CM7 owns SPI1, FDCAN1 and I2C4, so the CM7 is the
// gateway and the CM4 the backend. This header holds the operations once, for either core.
//
// A hardware semaphore can be released only by the core that took it. The handshake therefore uses, for each direction, one semaphore owned by the poster (POSTED) and one owned by the
// consumer (DONE), and every core releases only what it took itself:
//   post    (P): wait until POSTED and DONE are both free; fill the frame (zeros after the length), seal it, data barrier, take POSTED;
//   consume (C): POSTED taken and DONE free: copy the frame, check the seal, take DONE (a damaged frame is reported and dropped, and acknowledged all the same so the link does not stall);
//   finish  (P): POSTED taken by me and DONE taken: release POSTED;       finish (C): POSTED free and DONE taken by me: release DONE.
// The finishing steps are done by Ipc_Pump, which every operation calls, so a core that polls keeps its half of the handshake moving. A post that finds the channel not free within the time
// returns IPC_BUSY WITHOUT touching the frame (the single-writer rule: a frame is written only while the other core cannot be reading it).
//
// The hardware (the semaphore, the data barrier, the millisecond tick and the watchdog refresh) is reached through the callbacks of IpcOps, one table for each core. Nothing here has run on a board.
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
    IPC_BUSY = 2,      /* the channel was not free in time (or a semaphore could not be taken): nothing was written */
    IPC_TIMEOUT = 3,   /* no answer in time */
    IPC_DAMAGED = 4    /* a frame whose seal does not match was dropped */
} IpcStatus;

typedef enum { IPC_ROLE_GATEWAY = 0, IPC_ROLE_BACKEND = 1 } IpcRole;

typedef struct {
    uint32_t (*tick_ms)(void *ctx);
    void (*refresh)(void *ctx);                      /* the watchdog refresh, called while waiting */
    int (*sem_taken)(void *ctx, uint32_t channel);
    int (*sem_take)(void *ctx, uint32_t channel);    /* 1 when this core took it, 0 when it could not */
    void (*sem_release)(void *ctx, uint32_t channel);/* releases a semaphore THIS core took; the hardware ignores a release from the other core */
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

/* the poster's last step: the consumer has read the frame, so the semaphore the poster took is released */
static inline void Ipc_FinishPost(const IpcOps *ops, void *ctx, uint32_t posted, uint32_t done)
{
    if (ops->sem_taken(ctx, posted) && ops->sem_taken(ctx, done)) ops->sem_release(ctx, posted);
}

/* the consumer's last step: the poster has let go, so the semaphore the consumer took is released */
static inline void Ipc_FinishConsume(const IpcOps *ops, void *ctx, uint32_t posted, uint32_t done)
{
    if (!ops->sem_taken(ctx, posted) && ops->sem_taken(ctx, done)) ops->sem_release(ctx, done);
}

/* Both halves of the handshake of one core: the gateway posts commands and consumes answers, the backend consumes commands and posts answers. Every operation calls this; a loop that
 * only waits may call it too. */
static inline void Ipc_Pump(const IpcOps *ops, void *ctx, IpcRole role)
{
    if (role == IPC_ROLE_GATEWAY) {
        Ipc_FinishPost(ops, ctx, IPC_HSEM_CMD_ID, IPC_HSEM_CMD_DONE_ID);
        Ipc_FinishConsume(ops, ctx, IPC_HSEM_RESP_ID, IPC_HSEM_RESP_DONE_ID);
    } else {
        Ipc_FinishConsume(ops, ctx, IPC_HSEM_CMD_ID, IPC_HSEM_CMD_DONE_ID);
        Ipc_FinishPost(ops, ctx, IPC_HSEM_RESP_ID, IPC_HSEM_RESP_DONE_ID);
    }
}

static inline IpcStatus Ipc_Post(const IpcOps *ops, void *ctx, IpcRole role, volatile IpcFrame_t *frame, uint32_t posted, uint32_t done, uint8_t *sequence, uint8_t type, const uint8_t *data,
                                 uint8_t dlc, uint32_t wait_ms)
{
    const uint32_t start = ops->tick_ms(ctx);
    for (;;) {
        Ipc_Pump(ops, ctx, role);
        if (!ops->sem_taken(ctx, posted) && !ops->sem_taken(ctx, done)) break;
        ops->refresh(ctx);
        if (ops->tick_ms(ctx) - start > wait_ms) return IPC_BUSY;   /* the frame is not touched */
    }
    Ipc_Fill(frame, type, data, dlc, (*sequence)++);
    ops->barrier(ctx);
    return ops->sem_take(ctx, posted) ? IPC_OK : IPC_BUSY;
}

static inline IpcStatus Ipc_Take(const IpcOps *ops, void *ctx, IpcRole role, const volatile IpcFrame_t *frame, uint32_t posted, uint32_t done, IpcMessage *out)
{
    Ipc_Pump(ops, ctx, role);
    if (!ops->sem_taken(ctx, posted) || ops->sem_taken(ctx, done)) return IPC_NONE;
    Ipc_Copy(frame, out);
    const uint8_t valid = IpcFrame_Valid(frame);
    ops->barrier(ctx);
    (void)ops->sem_take(ctx, done);   /* acknowledged, damaged or not: the poster must be able to go on */
    return valid ? IPC_OK : IPC_DAMAGED;
}

/* gateway: post a command */
static inline IpcStatus Ipc_PostCommand(const IpcOps *ops, void *ctx, IpcMailbox_t *mailbox, uint8_t *sequence, uint8_t type, const uint8_t *data, uint8_t dlc)
{
    return Ipc_Post(ops, ctx, IPC_ROLE_GATEWAY, &mailbox->cmd, IPC_HSEM_CMD_ID, IPC_HSEM_CMD_DONE_ID, sequence, type, data, dlc, IPC_POST_COMMAND_WAIT_MS);
}

/* backend: take a command, if one is posted */
static inline IpcStatus Ipc_TakeCommand(const IpcOps *ops, void *ctx, IpcMailbox_t *mailbox, IpcMessage *out)
{
    return Ipc_Take(ops, ctx, IPC_ROLE_BACKEND, &mailbox->cmd, IPC_HSEM_CMD_ID, IPC_HSEM_CMD_DONE_ID, out);
}

/* backend: post an answer */
static inline IpcStatus Ipc_PostResponse(const IpcOps *ops, void *ctx, IpcMailbox_t *mailbox, uint8_t *sequence, uint8_t type, const uint8_t *data, uint8_t dlc)
{
    return Ipc_Post(ops, ctx, IPC_ROLE_BACKEND, &mailbox->resp, IPC_HSEM_RESP_ID, IPC_HSEM_RESP_DONE_ID, sequence, type, data, dlc, IPC_POST_RESPONSE_WAIT_MS);
}

/* gateway: take an answer, if one is posted (does not wait) */
static inline IpcStatus Ipc_TakeResponse(const IpcOps *ops, void *ctx, IpcMailbox_t *mailbox, IpcMessage *out)
{
    return Ipc_Take(ops, ctx, IPC_ROLE_GATEWAY, &mailbox->resp, IPC_HSEM_RESP_ID, IPC_HSEM_RESP_DONE_ID, out);
}

/* gateway: wait for an answer for a time */
static inline IpcStatus Ipc_WaitResponse(const IpcOps *ops, void *ctx, IpcMailbox_t *mailbox, IpcMessage *out)
{
    const uint32_t start = ops->tick_ms(ctx);
    while (ops->tick_ms(ctx) - start < IPC_WAIT_RESPONSE_MS) {
        ops->refresh(ctx);
        const IpcStatus status = Ipc_TakeResponse(ops, ctx, mailbox, out);
        if (status != IPC_NONE) return status;
    }
    return IPC_TIMEOUT;
}

#endif // HYDRA_IPC_LINK_H
