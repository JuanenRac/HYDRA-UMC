/*
 * =============================================================================
 * ipc_driver.h - CM5 <-> STM32H745 SPI link (userspace)
 * PROJECT: HYDRA-UMC
 * AUTHOR: JuanenRac (Electro Hobby 3D) - electrohobby3d@gmail.com
 * LICENSE: GPL-3.0 - see repo root LICENSE
 *
 * STARTING POINT ONLY - see README.md in this folder for the full status.
 * Linux-specific (spidev + libgpiod) - will not build on a non-Linux host.
 * =============================================================================
 */
#ifndef HYDRA_IPC_DRIVER_H
#define HYDRA_IPC_DRIVER_H

#include <stdint.h>
#include <stddef.h>

#define HYDRA_IPC_FRAME_SIZE 128 /* matches README.md section 10 - TBD final layout */

typedef struct {
  const char *spi_device;      /* e.g. "/dev/spidev0.0" */
  const char *data_ready_chip; /* e.g. "/dev/gpiochip0" */
  unsigned int data_ready_line;
  uint32_t spi_speed_hz;       /* up to 50,000,000 per README.md section 10 */
} hydra_ipc_config_t;

typedef struct hydra_ipc_handle hydra_ipc_handle_t;

/* Opens the SPI device and the HYDRA_DATA_READY GPIO line. Returns NULL on
 * failure (errno set). TODO: currently does not configure SPI mode/bits per
 * hop - defaults assumed, not yet verified against real STM32H745 SPI1
 * slave-mode config. */
hydra_ipc_handle_t *hydra_ipc_open(const hydra_ipc_config_t *cfg);

/* Blocks until HYDRA_DATA_READY asserts (or timeout_ms elapses, 0 = forever),
 * then reads one HYDRA_IPC_FRAME_SIZE-byte frame via SPI DMA-backed
 * transfer. Returns 0 on success, -1 on error/timeout (errno set).
 * TODO: does not yet parse the frame - caller gets raw bytes only. */
int hydra_ipc_read_frame(hydra_ipc_handle_t *h, uint8_t out[HYDRA_IPC_FRAME_SIZE], int timeout_ms);

void hydra_ipc_close(hydra_ipc_handle_t *h);

#endif /* HYDRA_IPC_DRIVER_H */
