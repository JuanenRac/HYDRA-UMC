# The display on the STM32H757

The DSI display of HYDRA-UMC hangs from the STM32H757, not from the host computer, and the H757 runs the **whole interface** on it. The machine can then be watched, stopped and put in a safe state from the panel even when the NVIDIA Jetson Orin NX has crashed or is still starting. This document is the design of that interface. **Nothing here has been built on a board**: what exists is the data of the screens, a view-model with host tests, a reference rendering and a budget; the firmware that draws is a later phase behind a build switch that ships off. No statement in this repository says the interface was validated on a display, a board or against a standard.

## What it is for

| Need | How the design meets it |
|---|---|
| Stop the machine whatever the Jetson does | STOP is a button of every screen, large, sent on the first touch, never refused and never behind a menu |
| See the state of the machine without the Jetson | status bar, dashboard, position, metrics, system and alarms read the stack bus and the H757 itself |
| Say clearly what is not working | the degraded mode names the cause (Jetson not answering, link fault, stack bus off) and every refusal has a reason on the screen |
| Not move anything on a guess | with the Jetson gone nothing moves and the stop is not released from the panel unless two switches say so (both off) |
| Same screens as the app | one data file of screens and texts, read by the firmware tables and by the Flutter app |

## Architecture

```
          ┌────────────── STM32H757 ──────────────┐            ┌──────────────────┐
  touch ─▶│ CM7: interface task  ──▶ LTDC/DSI ───▶│─▶ display  │  NVIDIA Jetson   │
 (I2C)    │        ▲        │                      │            │  Orin NX 16 GB   │
          │  view-model (ui_core)                  │◀──SPI1────▶│  vision, AI,     │
          │        │        ▼                      │  (master)  │  server, safety  │
 stack ◀─▶│ CM7: FDCAN1 stack bus, stop chain      │            │  zones           │
  bus     └────────────────────────────────────────┘            └──────────────────┘
```

* The CM7 owns the display peripherals (FMC with the SDRAM, LTDC, DSI host, DMA2D), the stack bus (FDCAN1) and the SPI link to the Jetson (SPI1, the H757 is the master). The view-model `src/ui_h757/core/ui_core.c` has no hardware in it: events in, effects out.
* The framebuffers are in the external SDRAM; the fonts and images are in the NOR flash on SPI5 (not memory mapped) and are copied to the SDRAM after their hash is checked. The budget is in `docs/UI_STACK_H757.md`.
* **The stop path does not run in the interface task.** A touch of STOP is turned into the stop request by the view-model, but the stack bus task that sends the stop frame (`docs/ESTOP_CAN.md`) and the physical input have priority over any drawing; a frozen drawing library must not delay a stop. The phase of the firmware that connects them has to prove this with the scheduler traces of the bench.
* There is **no remote session**: the panel has no network stack, no sign-in and no way to be driven from outside. What it asks of the stack goes over the bus; what it asks of the Jetson goes over the SPI link.

### What works without the Jetson, and what does not

| Works | Does not work (shown as unavailable, never as good) |
|---|---|
| STOP, always | the camera thumbnail (shown as unavailable) |
| Status of the stack bus, errors, bus off | the safety-zones verdict (it reads "not confirmed") |
| Stop latch, release of the stop (switch, off by default) | CPU load and temperature of the Jetson |
| Position and motion flag of the brain | any command that needs vision or the server |
| Jog (switch, off by default) | the camera and the server tabs of the app |
| Alarms, acknowledge, language, listen-only | |
| Metrics of the H757 | |

## Modes

The mode is decided by `tools/ui_model.py` (reference) and `src/ui_h757/core/ui_core.c` (the C twin, proved equal on every scenario). Priority from the top:

| Mode | When | What the panel allows |
|---|---|---|
| Stopped | the stop is latched, or STOP was touched and the brain has not reported the latch yet | STOP, navigation, acknowledge, settings; release with a long press when allowed |
| Alarm | an active alarm of danger or fault level (bus off, an object in a danger zone, the safety service inhibited or faulty, motion after STOP, STOP not confirmed) | STOP and navigation; no jog |
| Degraded | no valid heartbeat from the Jetson for `jetson_timeout_ms`, with the cause told apart: not answering, or the link failing (`spi_fault_after_errors` failed transactions in a row) | STOP, status, alarms; no jog and no release unless `degraded_allow_motion` / `degraded_allow_release` are set |
| Starting | no heartbeat yet and inside `jetson_boot_grace_ms` (a Jetson takes tens of seconds to start) | STOP and status; no jog |
| Normal | the Jetson answers, the bus is fine | everything the rules below allow |

Listen-only is not a mode but a switch that sits on top of all of them. The banner of the footer shows the worst condition; the status bar always shows the mode, the Jetson link, the stack bus and the number of alarms, each with a word as well as a colour.

The alarms and their level: Jetson not answering (warning), Jetson link faulty (warning), bus errors rising (warning), bus off (fault), STOP not confirmed (fault), motion after STOP (fault), and from the Jetson's safety verdict: warning (warning), danger (danger), inhibited (danger), fault (fault). An alarm is acknowledged to silence it on the screen; it does not disappear until its cause does.

### Values (`config/ui.cfg`)

All the numbers are in the configuration file, none in the code. They are starting values for the bench: heartbeat timeout 1500 ms, boot grace 90 s, link fault after 5 failed transactions, STOP unconfirmed after 500 ms, motion after STOP after 1000 ms, long press 1500 ms (release and leaving listen-only), jog armed for 10 s, bus warning at 96 errors. The two switches that let the degraded panel release the stop or jog are `0`; they are security options and go to the Studio with the conservative default.

## Safety rules

1. **STOP** is allowed in every mode, on the first touch, with no confirmation, and repeated touches repeat it. Tests: every scenario with STOP, and a fuzz of 200,000 events in C and 30,000 in Python that holds "STOP is never refused".
2. **Listen-only** (the same idea as the app): every command but STOP is refused with its reason, and so is the release of the stop. Turning it on is one touch; turning it off needs a long press, so a stray touch cannot re-enable commands.
3. **Confirmations**: the release of the stop and leaving listen-only are long presses (the hold is cancelled if the finger lifts or the state stops allowing it). The jog has to be armed first and the arming expires.
4. **Nothing is shown as good that was not confirmed**: a STOP the brain has not confirmed turns the banner into "do not rely on it, use the physical stop"; with the Jetson gone the safety verdict reads "not confirmed".
5. **The panel never releases a stop by itself**: it only asks, and the brain still waits for every board (`docs/SPI_SYSTEM.md`, release request).
6. **No remote session** (see the architecture).
7. **Operator PIN, optional** (decision of the owner): a switch of `config/ui.cfg` (`operator_pin_enabled`, **off by default**; it goes to the Studio with the conservative default). With it on the panel starts locked and locks again after `pin_idle_ms` without a touch: every command except STOP is refused with the reason "locked", the banner says so and touching it opens the PIN pad (a modal screen); leaving listen-only also needs the PIN (turning it on never does). **STOP never asks for the PIN**, nor do the screens that only show. After `pin_max_failures` wrong PINs in a row the pad is locked out for `pin_lockout_ms`. The view-model never sees the digits: the port that owns the pad checks the PIN and only tells the core "right" or "wrong". How and where the PIN is stored and compared (a hash in the F-RAM or the flash) is a task of the driver phase, not designed here.

## Relation to the Flutter DSI app

`HYDRA-UMC-DSI` is today the app of the bench with the CM5: a Flutter program that talks to the HYDRA server over the network. In the target design the display is on the H757, so the app and the firmware are two renderers of the **same screens**:

* `src/ui_h757/spec/ui_spec.json` (closed schema `src/ui_h757/spec/ui_spec.schema.json`) lists the screens, the widgets, their positions in the 1280×720 frame, the signals they show and the actions they carry; `src/ui_h757/spec/i18n.json` has every text in the eight languages. The firmware tables (`src/ui_h757/core/ui_screens.h`, `ui_texts.h`) are generated from them; the app reads the same files. The texts that are the same as the app's are linked to its keys and a tool compares them (`python tools/ui_spec.py arb <l10n folder>`).
* Parity by screen: dashboard, control, camera, position (the 3D view of the app, without the 3D), metrics, system, alarms, settings and the PIN pad (the app has its own, `lib/ui/widgets/pin_pad.dart`). What the app has and the panel does not: the live camera video (the panel has a still thumbnail), the 3D scene, server options, the ecosystem restart, pairing and the audit view. These need the Jetson or the network and stay in the app.
* The camera: the panel shows a **still thumbnail** (decision of the owner): the Jetson sends a small JPEG a few times a second over the SPI link and the video stays on the Jetson. The memory and the link budget are in `docs/UI_STACK_H757.md` (273 KiB of the SDRAM, 4 % of the link). Without the Jetson the screen says the camera needs it. The exchange of the picture is proposed in simulation (below).

## Signals and contract

Every signal of a widget points at the real contract or at a contract declared pending (`tools/ui_spec.py` checks both): the stop latch at `ESTOP_STATE` and the error counters at `HEALTH_FRAME` of `docs/can_ids.json`, the stack bus at `docs/bus_contract.json`, and the signals of the Jetson at the frames of its link below. **Pending** (not invented): the position and motion flags the kinematic brain publishes to its own screen; until they are fixed the view-model takes them as plain inputs.

### The exchange with the Jetson (in simulation)

`src/ui_h757/spec/jetson_link.json` defines the frames of the SPI link where the H757 is the master, with the framing of the boot exchange (`docs/SPI_BOOT_MASTER.md`: sync, type, sequence, length, payload up to 260 bytes, CRC-32) and types that do not collide with it. `STATUS_REQ` is answered by `STATUS` (the verdict of the safety-zones service, the load and temperature of the Jetson, whether a camera picture is available, its uptime): a valid `STATUS` is the heartbeat the view-model waits for, a damaged one or none is an SPI error. `SNAPSHOT_REQ` asks for the chunk at an offset of the current picture and is answered by `SNAPSHOT` (a chunk of a JPEG, in order) or `SNAPSHOT_NONE`; the Jetson keeps no state per request, so a lost chunk is simply asked again, and a picture that has a gap, a repeat, a wrong total or no JPEG markers is dropped whole. `tools/ui_link.py` is the reference of both sides with a Jetson double that can answer, stay silent, corrupt every answer or hang in the middle of a picture; `tests/test_ui_link.py` replays all of that, drives the view-model to the degraded mode and back, and checks that the picture traffic, framing included, stays under the share of the link that `config/ui.cfg` allows (about 4.5 % of a 10 MHz link at two pictures a second). This is a proposal in simulation: no Jetson has spoken it, and the Jetson side has to be written.

### The operator PIN store

`tools/ui_pin.py` is the reference of the store and of the pad (the port of the view-model; the view-model never sees digits) and `src/ui_h757/core/ui_pin.c` is its C twin: the same two slots of 72 bytes in the external F-RAM with a salted PBKDF2-HMAC-SHA256 hash (never the digits), a failure counter and a CRC; the valid slot with the newer sequence wins, so a write torn by a power cut leaves the old record. The failure counter is written **before** the comparison, so cutting the power cannot buy a free try, and it survives a restart: at start the port tells the view-model how many wrong PINs the store has counted (`PIN_FAILURES`) and at or above the limit the lock-out begins at once. A record that cannot be read never lets a PIN in and the panel stays locked until the Studio resets it (STOP does not depend on it). The comparison looks at every byte. The C test builds the same records byte for byte as the reference (hash vectors, the F-RAM after two `set` calls, the results of ten attempts), cuts the power at many points of a write, flips every byte of a record, wraps the sequence number and runs under the sanitizers; the C code also counts its SHA-256 compressions exactly.

`tools/ui_pin_cost.py` turns that count into time: one check is 2 x iterations + 2 compressions (20002 for the 10000 iterations of `config/ui.cfg`), about 130 ms in software at 400 MHz with 2600 cycles per compression, about 10 ms if the hash peripheral of the H757 were used. **The cycles per compression and the clock are estimates, not measurements; they have to be measured on the board.** With 3 tries per 30 s lock-out, trying every 4-digit PIN would take about 28 hours of lock-outs, and a restart does not reset the count. The PIN pad is not the stop path: nothing waits for it.

### The Jetson side

`tools/ui_jetson_side.py` is the Jetson side of the exchange as a protocol-level service: it answers the status and the camera picture from a status function and a source of cameras (the newest JPEG of each and its number), keeps no state per request, ignores what it cannot trust (damaged or unknown requests, a status request with a payload, a short snapshot request), never says a value the panel cannot read (an unknown verdict is `unknown`, the load is capped at 100) and gives the master reference the same heartbeats and pictures as the double. What a real Jetson still needs (the SPI slave endpoint, the safety-zones service, the camera capture) is not written.

### The master side in C

`src/ui_h757/core/ui_link.c` is the twin of the master side: it polls the status, fetches the picture chunk by chunk into a buffer of the caller and reports events for the view-model. Seven transcripts were recorded from the Python reference (a good Jetson, a newer picture, silence and return, a corrupting link, a hang in the middle of a picture, one damaged chunk, a picture that changes in the middle of a transfer); the C master has to send the same request bytes at the same moments, report the same events and end with the same pictures and counters.

### The runtime and the stop path (in simulation)

`src/ui_h757/core/ui_runtime.c` ties the view-model, the link master and the PIN store to the ports a board gives (the SPI transaction, send stop, send release, send jog, save a setting) and turns the effects of the view-model into calls of those ports; the F-RAM ports of the PIN store are the driver of the brain (`fram_i2c.h`) at the end of the chip, below the scratch area of the self-test (`ui_pin_fram.h`). `ui_runtime_touch` with STOP calls `send_stop` **first**, with nothing before it and nothing that waits: it touches neither the SPI link nor the PIN nor the pad. The host test runs the whole chain against a Jetson double, the real F-RAM driver over a fake I2C bus and a machine double: the Jetson hangs and comes back, STOP goes out without a single request to the Jetson, the PIN locks and unlocks, the failures counted survive a restart, and **the F-RAM erase of the brain (zeros over the chip) reads as "no PIN" and lets nothing in** (this was found while writing the adapter: zeros used to read as a damaged record). A right PIN typed during a lock-out is not even checked, because the store would clear its counter.

`tools/ui_sched_sim.py` models the task priorities (the touch interrupt only leaves a request, the stack task sends the stop frame, the interface task draws, the PIN check is a long job): the time from a touch to the stop frame stays about 1 ms and does not grow with a 300 ms frame or the PIN check, and with the interface task above the stack task the stop is starved, so the argument depends on the priorities of the design. **The numbers are estimates and the model is not FreeRTOS: the latency is measured on the board.** `ui_pin_bench.h` is the harness that measures the cycles of one PIN check with the cycle counter of the core, and `ui_pin_cost.py calibrate` turns that measurement into the value for `config/ui.cfg`.

## Evidence

| What | Evidence |
|---|---|
| view-model, scenarios (Jetson hangs, SPI cut, bus errors, STOP while moving, listen-only, switches), fuzz | simulator (host tests in Python and in C, with sanitizers) |
| screen data, texts in eight languages, touch targets, contrast, links to the contract | simulator (data checks) |
| text fits in every box in every language | simulator (reference renderer) |
| memory and bandwidth budget | arithmetic on data; the size of the C core for the Cortex-M7 is measured in the CI |
| anything on the display, the board, the Jetson, the bus | **pending hardware**, listed below |

### Physical tests pending (nothing was done)

1. H757 board with the real display (landscape only): the screen shows the dashboard in the eight languages with legible text and touch targets at arm's length; the 30 Hz refresh does not flicker; the orientation (the panel is portrait natively) and the rotation cost.
2. Jetson hung on purpose (power it from a switchable supply and freeze it): the panel goes to the degraded mode within the timeout, STOP still stops, nothing moves.
3. SPI link cut (logic analyser on SCK, MOSI, MISO, NSS and READY): the panel names the link as the cause after the configured number of failures.
4. Stack bus with injected errors and a bus off: the banner and the refusals match the scenario of the same name.
5. STOP during a jog with the real axes: the time from touch to the stop frame and to the drivers off, with the interface task loaded by drawing.
6. Time to the first frame after power-up and during the Jetson start.
7. The camera thumbnail with the real Jetson: its rate over the SPI link and the JPEG decode time on the H757.
8. With the PIN switch on: the panel locks after the idle time, STOP works while locked and during the lock-out, and the PIN is not kept in clear.

## Phases of the firmware

Only in Actions, on temporary branches, behind the switch `HYDRA_UI_H757` that is `0` in every build as it ships; no version is raised and nothing is flashed. The renaming of the firmware folder, the platform profile and the SPI driver belong to their own work and this one does not touch those files.

1. **Data and core** (this repository today): screens, texts, view-model in Python and C, generated tables, tests, budget, reference pictures.
2. **Ports**: the thin interface between the core and the hardware (touch read, display flush, time, bus queue), with host doubles; the task and its priorities, proved with a scheduler trace on the host.
3. **Library**: LVGL pinned by commit, the widgets built from `ui_screens.h`, the fonts generated from the glyphs of the texts; built for the Cortex-M7 in Actions and its size compared with the budget.
4. **Display**: the FMC SDRAM start-up test, the LTDC/DSI configuration and the ILI9881 start-up sequence (candidates from the CubeMX project, not confirmed), DMA2D; bench only.
5. **Link**: the heartbeat and status exchange with the Jetson once its frame is defined; the position frame of the brain.

## Open decisions

| Decision | Default until the owner decides |
|---|---|
| CM7 application slot | decided: 768 KiB, the rest of bank 1 (`docs/UI_STACK_H757.md`); the CM7 linker script still has the 384 KiB of the earlier layout until the H757 script is generated from the profile |
| Fonts and images in the SPI5 NOR, copied to the SDRAM after a hash check | as written |
| Camera still on the panel | decided: a still thumbnail, not video; the exchange with the Jetson is proposed and tested in simulation (above) |
| Release of the stop and jog with the Jetson gone | refused (switches off) |
| Operator PIN on the panel | decided: optional, off by default, STOP never asks for it; the store and its C twin are tested in simulation (above); the cost of the hash on the Cortex-M7 is an estimate to measure |
| Model and size of the external SDRAM (the owner will add RAM) | **pending: to be confirmed by the owner**; `sdram_bytes` of `config/ui.cfg` (one bank of 128 MiB today, marked not confirmed) |
| Orientation | decided: landscape only; how the rotation of the natively portrait panel is done (library or DMA2D) is for the bench |
