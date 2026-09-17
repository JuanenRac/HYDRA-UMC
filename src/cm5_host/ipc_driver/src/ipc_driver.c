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
 * DONE: hydra_ipc_open()/hydra_ipc_read_frame() now really request
 * HYDRA_DATA_READY via libgpiod v2's edge-event API and block on a real
 * rising edge before the SPI transfer, instead of polling the bus
 * unconditionally - still unverified against real hardware (no STM32H745
 * firmware exists yet to assert the line), but the handshake itself is
 * real code now, not a stub.
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
#include <time.h>

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

  /* Requests HYDRA_DATA_READY as an input with rising-edge detection via
   * libgpiod v2's request API (this project's own target OS image ships
   * libgpiod v2 - see ../../../os/README.md). hydra_ipc_read_frame() below
   * waits on this same request for a real edge event instead of polling
   * the SPI bus unconditionally. */
  struct gpiod_line_settings *settings = gpiod_line_settings_new();
  if (!settings) {
    gpiod_chip_close(h->chip);
    close(h->spi_fd);
    free(h);
    return NULL;
  }
  gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_INPUT);
  gpiod_line_settings_set_edge_detection(settings, GPIOD_LINE_EDGE_RISING);

  struct gpiod_line_config *line_cfg = gpiod_line_config_new();
  struct gpiod_request_config *req_cfg = gpiod_request_config_new();
  if (!line_cfg || !req_cfg ||
      gpiod_line_config_add_line_settings(line_cfg, &cfg->data_ready_line, 1, settings) < 0) {
    if (req_cfg) gpiod_request_config_free(req_cfg);
    if (line_cfg) gpiod_line_config_free(line_cfg);
    gpiod_line_settings_free(settings);
    gpiod_chip_close(h->chip);
    close(h->spi_fd);
    free(h);
    return NULL;
  }
  gpiod_request_config_set_consumer(req_cfg, "hydra_ipc_driver");

  h->data_ready_req = gpiod_chip_request_lines(h->chip, req_cfg, line_cfg);
  gpiod_request_config_free(req_cfg);
  gpiod_line_config_free(line_cfg);
  gpiod_line_settings_free(settings);
  if (!h->data_ready_req) {
    gpiod_chip_close(h->chip);
    close(h->spi_fd);
    free(h);
    return NULL;
  }

  return h;
}

int hydra_ipc_read_frame(hydra_ipc_handle_t *h, uint8_t out[HYDRA_IPC_FRAME_SIZE], int timeout_ms)
{
  if (!h || !out) {
    errno = EINVAL;
    return -1;
  }

  /* Real handshake: block until HYDRA_DATA_READY asserts (rising edge) or
   * timeout_ms elapses (0 per hydra_ipc_config_t contract means forever -
   * pass NULL for the timeout struct in that case, libgpiod's own "wait
   * forever" convention). Reading a frame off the bus before this line
   * asserts risks a torn/stale transfer from the STM32H745 side. */
  struct timespec ts;
  struct timespec *wait_ts = NULL;
  if (timeout_ms > 0) {
    ts.tv_sec = timeout_ms / 1000;
    ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
    wait_ts = &ts;
  }
  int ready = gpiod_line_request_wait_edge_events(h->data_ready_req, wait_ts);
  if (ready < 0) {
    return -1;
  }
  if (ready == 0) {
    errno = ETIMEDOUT;
    return -1;
  }
  struct gpiod_edge_event_buffer *events = gpiod_edge_event_buffer_new(1);
  if (!events) {
    return -1;
  }
  int read_count = gpiod_line_request_read_edge_events(h->data_ready_req, events, 1);
  gpiod_edge_event_buffer_free(events);
  if (read_count < 0) {
    return -1;
  }

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
