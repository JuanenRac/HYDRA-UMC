/*
 * =============================================================================
 * ipc_driver.c - CM5 <-> STM32H745 SPI link (userspace)
 * PROJECT: HYDRA-UMC
 * AUTHOR: JuanenRac (Electro Hobby 3D) - electrohobby3d@gmail.com
 * LICENSE: GPL-3.0 - see repo root LICENSE
 *
 * STARTING POINT ONLY - proves the shape of the spidev + libgpiod interface
 * this driver will need, NOT a verified-working link (no real STM32H745
 * firmware exists yet on the other end - see ../../mcu_stm32h745/README.md).
 * Not compiled/verified on this development machine (Windows) - Linux-only
 * dependencies (linux/spi/spidev.h, gpiod.h). Verify on target or a
 * cross-compile sysroot before trusting this builds cleanly.
 *
 * TODO, tracked against docs/architecture.md and README.md section 10:
 *   - Real 128-byte frame format parsing (not defined yet)
 *   - Tier-0 SPI-OTA bootloader client (architecture.md section 2)
 *   - Verify SPI mode/bit order against the real STM32H745 SPI1 slave config
 *     once that firmware exists (assumed Mode 0, MSB-first below - not
 *     confirmed against real hardware)
 * =============================================================================
 */

#include "ipc_driver.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>
#include <gpiod.h>
#include <poll.h>

struct hydra_ipc_handle {
  int spi_fd;
  struct gpiod_chip *chip;
  struct gpiod_line_request *data_ready_req;
  uint32_t spi_speed_hz;
};

hydra_ipc_handle_t *hydra_ipc_open(const hydra_ipc_config_t *cfg)
{
  if (!cfg || !cfg->spi_device || !cfg->data_ready_chip) {
    errno = EINVAL;
    return NULL;
  }

  hydra_ipc_handle_t *h = calloc(1, sizeof(*h));
  if (!h) return NULL;

  h->spi_fd = open(cfg->spi_device, O_RDWR);
  if (h->spi_fd < 0) {
    free(h);
    return NULL;
  }

  uint8_t mode = SPI_MODE_0; /* TODO: confirm against real STM32H745 SPI1 slave config */
  uint8_t bits = 8;
  h->spi_speed_hz = cfg->spi_speed_hz ? cfg->spi_speed_hz : 10000000; /* conservative default, not the full 50 MHz until verified */

  if (ioctl(h->spi_fd, SPI_IOC_WR_MODE, &mode) < 0 ||
      ioctl(h->spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
      ioctl(h->spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &h->spi_speed_hz) < 0) {
    close(h->spi_fd);
    free(h);
    return NULL;
  }

  h->chip = gpiod_chip_open(cfg->data_ready_chip);
  if (!h->chip) {
    close(h->spi_fd);
    free(h);
    return NULL;
  }

  /* TODO: request the HYDRA_DATA_READY line as an input with edge-rising
   * detection via libgpiod v2's request API - left unimplemented pending
   * confirmation of which libgpiod version this project's own target OS
   * image ships (see ../../../os/README.md). h->data_ready_req stays NULL
   * for now; hydra_ipc_read_frame() below falls back to a plain SPI poll
   * without waiting on the interrupt line. */
  (void)cfg->data_ready_line;

  return h;
}

int hydra_ipc_read_frame(hydra_ipc_handle_t *h, uint8_t out[HYDRA_IPC_FRAME_SIZE], int timeout_ms)
{
  if (!h || !out) {
    errno = EINVAL;
    return -1;
  }

  /* TODO: wait on HYDRA_DATA_READY (h->data_ready_req) before transferring,
   * once that's implemented above - polling the SPI bus unconditionally
   * like this is a placeholder, not the real handshake. */
  (void)timeout_ms;

  struct spi_ioc_transfer tr;
  memset(&tr, 0, sizeof(tr));
  tr.tx_buf = 0;
  tr.rx_buf = (unsigned long)(uintptr_t)out;
  tr.len = HYDRA_IPC_FRAME_SIZE;
  tr.speed_hz = h->spi_speed_hz;
  tr.bits_per_word = 8;

  if (ioctl(h->spi_fd, SPI_IOC_MESSAGE(1), &tr) < 1) {
    return -1;
  }

  /* TODO: parse `out` per the real frame format once defined - caller gets
   * raw bytes only today. */
  return 0;
}

void hydra_ipc_close(hydra_ipc_handle_t *h)
{
  if (!h) return;
  if (h->data_ready_req) gpiod_line_request_release(h->data_ready_req);
  if (h->chip) gpiod_chip_close(h->chip);
  if (h->spi_fd >= 0) close(h->spi_fd);
  free(h);
}
