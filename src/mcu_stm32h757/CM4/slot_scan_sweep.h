/*
 * =============================================================================
 * slot_scan_sweep.h - one full slot scan: the eight windows, one after the other (HAL-free, host-testable)
 * PROJECT: HYDRA-UMC
 * AUTHOR: JuanenRac (Electro Hobby 3D) - electrohobby3d@gmail.com
 * LICENSE: GPL-3.0-or-later - see repo root LICENSE
 *
 * The scan asked for through the SPI link is eight windows of docs/SLOT_SCAN.md. For each slot this file begins the window (slot_scan_master.h), lets the
 * bus layer run it (send the probe, hand in every frame heard and every error frame, for the time of the window) through a function the caller gives, and takes
 * the verdict. The function is the only part that touches the bus (KinematicBrainCan_ScanWindow in the firmware, a stand-in in tests/test_slot_scan_sweep.c),
 * so the order of the windows, the scan id and the verdicts that reach the CM5 are checked on the host. The verdicts are the codes of the SPI link
 * (0 absent, 1 ok, 2 duplicate, 3 misplaced, 4 noisy: tools/spi_system.py).
 * =============================================================================
 */
#ifndef SLOT_SCAN_SWEEP_H
#define SLOT_SCAN_SWEEP_H

#include <stdint.h>
#include "slot_scan_master.h"

/* Runs the window of one slot: sends the probe of scan_id to the slot and, for the length of the window, calls SlotScanMaster_OnFrame for each frame heard on
 * the reply identifier of the slot and SlotScanMaster_OnBusError for each error frame. */
typedef void (*SlotScanWindowFn)(void *context, uint8_t slot, uint8_t scan_id, SlotScanMaster *window);

static inline void SlotScanSweep_Run(SlotScanWindowFn run_window, void *context, uint8_t scan_id, uint8_t verdicts[SLOT_SCAN_MASTER_SLOTS])
{
    for (uint8_t slot = 0u; slot < SLOT_SCAN_MASTER_SLOTS; slot++) {
        SlotScanMaster window;
        SlotScanMaster_Begin(&window, slot, scan_id);
        if (run_window != 0) run_window(context, slot, scan_id, &window);
        verdicts[slot] = (uint8_t)SlotScanMaster_Verdict(&window);
    }
}

#endif /* SLOT_SCAN_SWEEP_H */
