<p align="center">
  <img src="images/HYDRA_UMC_BANNER.svg" alt="HYDRA-UMC Banner" width="100%">
</p>

# 🚀 HYDRA-UMC TECHNICAL SPECIFICATION
### 🤖 The Ultimate Dual-Core Micro-Factory & Multi-Robot Controller Platform (V1.0 - PCIe Hailo-8 AI Accelerator & Dual USB 3.0 Hubs)

---

## 1. 🛠️ PROJECT OVERVIEW & THE MICRO-FACTORY ECOSYSTEM

**HYDRA-UMC** (Universal Multi-axis Controller) is an industrial-grade, distributed control platform and high-performance HMI architecture designed for multi-axis cellular robotics, micro-factories, automated manufacturing, and complex toolhead orchestration. 

Built on a **Heterogeneous Host + Real-Time Co-Processor Architecture**, HYDRA-UMC decouples high-level user interface rendering, computer vision, AI inference, and cloud connectivity from real-time step generation, fieldbus management, and power electronics actuation.

```mermaid
flowchart TB
    CM5["<b>Compute Module 5 (Host / Cerebro)</b><br/>Broadcom BCM2712 Quad Cortex-A76 @ 2.4 GHz<br/>VideoCore VII GPU (OpenGL ES 3.1 / Vulkan 1.2)<br/>RP1 Dual USB 3.0 Host Controllers (2x 5 Gbps)<br/>Linux OS with PREEMPT_RT patchset<br/>High-FPS touch UI (Qt6 / Flutter) via MIPI-DSI<br/>Trajectory planning, G-code parsing &amp; Vision AI"]

    CM5 -- "PCIe Gen 3.0 x1 (up to 8 Gbps)" --> HAILO["<b>Hailo-8 M.2 AI Accelerator</b><br/>26 TOPS neural coprocessor"]
    CM5 -- "USB3 Channel 1" --> HUB1["GL3523 Hub #1"]
    CM5 -- "USB3 Channel 2" --> HUB2["GL3523 Hub #2"]
    HUB1 --> CAM14["4x USB3 camera ports<br/>(Cam 1-4)"]
    HUB2 --> CAM58["4x USB3 camera ports<br/>(Cam 5-8)"]

    CM5 -- "High-Speed SPI bus + DMA + IRQ pin" --> MCU

    subgraph MCU["STM32H745ZIT6 Real-Time Co-Processor (LQFP-144)"]
        direction LR
        CM7["<b>Cortex-M7 @ 480 MHz</b><br/>S-Curve kinematics<br/>Hardware timers<br/>6-axis local stage"]
        CM4["<b>Cortex-M4 @ 240 MHz</b><br/>FDCAN1 controller<br/>Sensor filtering<br/>Inter-core IPC"]
    end
    MEM["1 MB SRAM / 2 MB dual-bank internal flash<br/>Dedicated SPI2 interface to 64 KB FRAM"]
    MCU --- MEM

    MCU -- "FDCAN1 - STACK A bus" --> ROBOTS["Robot Controller Boards A1...A8<br/>(up to 8 slave modules)"]
```

### 🤖 Micro-Factory Capabilities:
* 📡 **Distributed Multi-Robot Network:** Coordinates up to 8 distributed slave robotic modules (e.g., Parol6 robotic arms, toolheads, and auxiliary axes) connected over a single physical FDCAN bus.
* 🧠 **Embedded Neural Vision Supercomputing:** Onboard PCIe M.2 Hailo-8 Coprocessor (26 TOPS) enabling multi-stream YOLOv8/YOLO11 object detection, defect inspection, and real-time PnP fiducial alignment across all 8 cameras.
* 📐 **Local 6-Axis Stage:** Direct step/dir/enable pulse generation for 6 local axes (X, Y, Z, A, B, C) driving Cartesian positioning systems, indexers, or local gantries.
* 🎯 **JuanenPNP & JuanenCNC Integration:** Directly compatible with Pick-and-Place systems (LumenPNP hardware structures) and CNC units equipped with 10W optical laser modules for PCB prototyping and SMD placement.
* 👁️ **Octal Camera Vision & Inspection Matrix:** Integrated dual USB 3.0 controllers driving 8x dedicated USB camera ports for real-time OpenCV pick-and-place optical alignment, thermal inspection, and remote stream monitoring.
* ⚡ **Actuation Matrix & Thermal Management:** Controls 16 industrial low-side MOSFET channels (8 electropneumatic valves + 8 vacuum pumps/venturi generators) and high-current bed drivers for SMD reflow soldering or 3D printing beds.
* 🚜 **JuanenBOT Mobile Platforms:** Scalable communication architecture capable of interfacing with heavy-duty 48V 4-wheeled transport platforms (50x50x50 cm frames with omnidirectional/mecanum wheels for 100 kg payloads).

---

## 2. 🖥️ HOST COMPUTING SUBSYSTEM (HMI & HIGH-LEVEL)

* 🧩 **Module:** Raspberry Pi Compute Module 5 (CM5)
* ⚙️ **Processor:** Broadcom BCM2712 Quad-Core ARM Cortex-A76 @ 2.4 GHz
* 🎮 **Graphics Engine:** VideoCore VII GPU (OpenGL ES 3.1, Vulkan 1.2)
* 💾 **System Memory:** 2 GB / 4 GB LPDDR4X (Integrated on CM5)
* 💽 **High-Speed Storage:** Integrated eMMC Flash
* 🐧 **Operating System:** Linux 64-bit (Raspberry Pi OS / Yocto patched with `PREEMPT_RT`)
* 📺 **Display Interface:** MIPI-DSI (2-lane / 4-lane) connected to high-resolution capacitive touch panel (Bambu Lab style UI at 60 FPS)
* 🌐 **Connectivity Suite:**
  * 🌐 1x Gigabit Ethernet (RJ45) for industrial LAN / RTSP video streaming / WebSockets / MQTT
  * 📶 Wi-Fi 6 & Bluetooth 5.4
  * 📷 **8x USB 3.0 / 2.0 Vision Ports:** Driven by onboard dual Genesys Logic GL3523 controllers.
  * 🎮 **2x USB 2.0 HID Ports:** Gamepad / mouse / keyboard - see section 4a.

---

## 3. 🧠 PCIE AI ACCELERATOR SUBSYSTEM (HAILO-8 NPU)

* 🔌 **Physical Interface:** Onboard M.2 Key M socket (2242 / 2280 form factor) connected directly to the CM5 PCIe Gen 2.0 / 3.0 x1 bus.
* 🚀 **NPU Engine:** Hailo-8 Industrial AI Processor delivering **26 TOPS** (Tera Operations Per Second) at sub-5W power consumption.
* ⚡ **Software Integration:** Official Hailo RT software suite integrated with Raspberry Pi OS, executing GStreamer pipelines and OpenCV for zero-CPU-overhead neural inference.

---

## 4. 📷 DUAL USB 3.0 VISION SUBSYSTEM (8x CAMERA PORTS)

* 🎛️ **Hub Controllers:** 2x Genesys Logic `GL3523` USB 3.0 / SuperSpeed Hub ICs integrated directly on the motherboard.
* 🔀 **Topology & Distribution:**
  * 🅰️ **Hub #1 (`GL3523-A`):** Connected to CM5's native USB3-0 SuperSpeed PHY (5 Gbps). Feeds USB Ports 1 to 4 (Cameras A1-A4).
  * 🅱️ **Hub #2 (`GL3523-B`):** Connected to CM5's native USB3-1 SuperSpeed PHY (5 Gbps). Feeds USB Ports 5 to 8 (Cameras A5-A8).
  * ℹ️ CM5 exposes these 2 SuperSpeed PHYs directly (BCM2712) - no RP1 companion chip is involved (RP1 is specific to the Raspberry Pi 5 board, not CM5). Full pin-level signal routing: `docs/PINOUT_CM5_CARRIER.TXT`.
* 🛡️ **Power Switch & Circuit Protection:** Individual USB VBUS protection via high-side current-limiting power switches (`TPS2065` / `SY6280`) configured for 500 mA - 1 A with fault reporting.
* ⚡ **High-Current VBUS Rail:** Powered by a dedicated 24V to 5V Step-Down Regulator (5V @ 6A continuous).

### 4a. 🎮 USB 2.0 HID SUBSYSTEM (2x GAMEPAD / MOUSE / KEYBOARD PORTS)

* 🎛️ **Hub Controller:** 1x small USB 2.0 hub IC (e.g. Genesys Logic `GL850G` / `FE1.1s`, TBD) fanning CM5's single native USB 2.0 PHY out to 2 physical ports.
* ℹ️ **Why a hub is needed:** the CM5 datasheet (`docs/datasheets/Raspberry Pi CM5.pdf`, §2.5) confirms BCM2712 exposes exactly **one** USB 2.0 (High Speed) port at the DF40 connector (`USB_N`/`USB_P`, pins 103/105) - separate and distinct from the 2x native USB 3.0 SuperSpeed PHYs already dedicated to the GL3523 camera hubs (section 4). A single physical pair cannot be split into 2 ports without a hub in between.
* 🔀 **Topology:** `USB_N`/`USB_P` (CM5) -> hub upstream port -> 2x downstream USB 2.0 Type-A ports (front/side panel, for a gamepad, mouse, or keyboard - manual jog/teach-pendant control and HMI input, independent of the touchscreen).
* 📌 Full pin-level signal routing: `docs/PINOUT_CM5_CARRIER.TXT` section 1.

---

## 5. ⚡ REAL-TIME CO-PROCESSING SUBSYSTEM

* 🎛️ **Microcontroller:** STMicroelectronics **STM32H745ZIT6** (Cost-optimized dual-core MCU)
* 📦 **Package:** LQFP-144 (0.5 mm pin pitch)
* 🧠 **Architecture:** Dual-Core Asymmetric Multiprocessing (AMP)
  * 🚀 **Core 1 (Cortex-M7 @ 480 MHz):** Real-time motion engine, hardware pulse generation, S-curve kinematic velocity profiles, PID control loops.
  * 📡 **Core 2 (Cortex-M4 @ 240 MHz):** FDCAN protocol management, analog sensor filtering, safety interlocks, and inter-core IPC handling.
* 💾 **Internal Memory Architecture:**
  * 💾 **2 MB** Dual-Bank Internal Flash
  * 🧠 **1 MB** Total Internal SRAM (512 KB AXI SRAM + 128 KB ITCM / 128 KB DTCM + SRAM1/SRAM2/SRAM3)
* 🧵 **RTOS:** **FreeRTOS**, one independent instance per core (AMP, not SMP - no shared scheduler state between Core 1 and Core 2). Firmware skeleton: `src/mcu_stm32h745/`, see `docs/architecture.md` section 2.

---

## 6. 📡 DISTRIBUTED FIELDBUS COMMUNICATION (SINGLE FDCAN)

The motherboard acts as a master controller for up to 8 individual slave robotic modules distributed across a single physical CAN bus:

* 🔌 **Hardware Peripheral:** 1x Native Hardware FDCAN Controller (`FDCAN1`) built directly into the STM32H745, run in **Classic CAN mode** (`FDCAN_FRAME_CLASSIC`, `BRS_OFF`) by the real bootloader implementation - the peripheral is FD-capable silicon, but the CAN-OTA/SPI-OTA protocol this project actually speaks today (`docs/CANBUS_STM32H745.TXT`, `docs/CANBUS_STM32G474.TXT`) uses classic frames only (max DLC 8), same as every other tier (G474 Robot Controller Boards, URTC). CAN FD's larger 64-byte BRS payloads are real hardware headroom for later, not something the protocol uses yet.
* ⚡ **Physical Layer Transceiver:** 1x High-Speed CAN FD Transceiver (e.g., TI `TCAN1044AVD` / NXP `TJA1443`) - FD-capable hardware chosen for the same future-headroom reason as the peripheral above, even though today's traffic is classic frames.
* 🔀 **Bus Topology:**
  * 🅰️ **STACK A (`FDCAN1`):** Serves Slave Modules A1 through A8.
* ⏱️ **Protocol Specs:** ~1 Mbps nominal bitrate (Classic CAN, 8-byte max payload per frame). Auto-bus-off recovery is planned to be managed by the Cortex-M4 - not yet implemented in application firmware (today's CM4 `main.c` is a bring-up/blink skeleton, see `src/mcu_stm32h745/CM4/`), tracked as real future work rather than a shipped capability.
* 🔌 **Physical Connector:** 40-pin, 2.54mm-pitch STACKING header/socket (+24V ×10 pins, GND ×10 pins, +5V ×4 pins auxiliary, FDCAN1 H/L, `BOARD_PRESENT_N`, 13 spare) - the 8 Robot Controller Boards physically STACK one on top of another on one side of this board (CONFIRMED topology, not a backplane), each board straight-through-passing all 40 signals to whatever mounts above it. Slot addressing is a LOCAL DIP switch per board (`BOARD_ID[2:0]`, README.md section 12), not derived from this connector. Full pin table and stack topology in `docs/PINOUT_STACKA_CONNECTOR.TXT`. Identical connector definition on the Kinematic Brain's own port and every Robot Controller Board's pair of ports.

```mermaid
flowchart LR
    FDCAN1["STM32H745<br/>FDCAN1 Controller"] --> XCVR["TCAN1044<br/>Transceiver"] --> BUS["STACK A Bus<br/>(Robots A1 - A8)"]
```

---

## 7. 💾 ULTRA-FAST NON-VOLATILE MEMORY (SPI FRAM)

To guarantee zero data loss and instant state recovery during emergency power disruptions:

* 🧪 **Memory IC:** Cypress/Infineon `FM25V05-G` / Fujitsu `MB85RS64` (64 KB SPI FRAM)
* ⚡ **Bus Interface:** Dedicated SPI2 bus up to 40 MHz.
* ♾️ **Durability:** Infinite endurance (10^14 cycles) with nanosecond write latencies.
* 🛡️ **Anti-Power Loss Sequence (PVD):** The internal Power Voltage Detector (PVD) monitors the 3.3V rail. Upon voltage drop detection, a Non-Maskable Interrupt (NMI) dumps encoder vectors, active state machines, and coordinates to FRAM in under **5 microseconds** before supply shutdown.

---

## 8. 🦾 LOCAL MOTION, ACTUATION & SENSOR SUITE

### ⚙️ Motion Outputs
* 🎯 **Supported Axes:** 6-Axis Local Stage - dual-Y gantry + tool axes (`X`, `Y1`, `Y2`, `Z`, `E0`, `E1`), driven by 6x TMC5160A stepper drivers in an SPI daisy-chain.
* ⚡ **Signals:** 3.3V CMOS (`STEP`, `DIR`, `ENABLE`), shared SPI4 daisy-chain to all 6 drivers.
* ⏱️ **Timers:** Advanced-Control Timers (`TIM1` for X/Y1/Y2/Z, `TIM8` for E0/E1) with hardware pulse generation.
* 🛑 **Endstops:** 12x inputs, 2 per axis (MIN + MAX).
* 📌 Full pin-level allocation: `docs/PINOUT_STM32H745_KINEMATIC_BRAIN.TXT`.

### 🔌 Power & Fluidic Actuators
* 🔀 **20x Low-Side Switching Channels:** Industrial N-Channel MOSFET outputs with flyback protection.
  * 🧲 **8+2 Channels:** Vacuum Pumps / Venturi Pick-and-Place generators.
  * 💨 **8+2 Channels:** Electropneumatic Valves (5V/24V actuation).
* 💨 **Fans:** 3x 3-wire fans (PWM-switched supply via low-side MOSFET + tachometer sense per channel).
* 🌡️ **Thermal Management:**
  * 🔥 1x Solid-State Relay control output for the Heated Bed, switching **230VAC mains** - opto-isolated from the MCU/logic domains; this is a mains-voltage circuit and needs real creepage/clearance on the PCB, not a 24V-bus footprint.
  * 🌡️ 2x Precision NTC Thermistor analog inputs (heated bed) sampled by `ADC1`.

---

## 9. 🔌 POWER DISTRIBUTION & REGULATION

The board operates from a single industrial **24V DC** input supply bus:

* ⚡ **Main DC Input:** 24V DC ±10%
* 🔋 **5V Main Power Domain:** Step-down synchronous buck regulator supplying **5A continuous** for the CM5 module, touchscreen display backlight, and onboard logic.
* 📷 **5V USB VBUS Power Domain:** Dedicated synchronous buck regulator supplying **6A continuous** exclusively for the 8x USB 3.0 camera ports and GL3523 hub controllers.
* 🎛️ **3.3V Power Domain:** Low-noise regulator supplying **4A continuous** (dimensioned for STM32, FRAM, transceivers, and the 3.3V rail of the M.2 Hailo-8 socket).

---

## 10. 🔄 INTER-PROCESSOR COMMUNICATION (IPC)

Communication between CM5 (Host) and STM32H745 (Co-Processor) utilizes a hardware-assisted, zero-copy SPI link:

* 🔗 **Physical Transport:** Full-Duplex SPI1 running up to 50 MHz in Slave Mode on STM32 and Master Mode on CM5.
* 🤝 **Handshake Line:** `HYDRA_DATA_READY` GPIO line.
* ⚡ **Execution Flow:** The Cortex-M4 prepares a 128-byte telemetry frame in shared AXI SRAM, asserts `HYDRA_DATA_READY`, and the CM5 fetches the packet via high-speed SPI DMA without polling overhead.

---

## 11. 🎛️ 4-LAYER PCB HARDWARE SPECIFICATIONS

* 📐 **Form Factor:** Monolithic Industrial Motherboard.
* 🥞 **Layer Stackup (4-Layer):**
  * 🟢 **Layer 1 (Top):** Component placement, high-frequency signals, 90-ohm USB SuperSpeed differential pairs, 85-ohm PCIe Gen 3.0 pairs.
  * 🛡️ **Layer 2 (Inner 1):** Continuous solid Ground Plane (`GND`).
  * ⚡ **Layer 3 (Inner 2):** Split Power Planes (`24V`, `5V_MAIN`, `5V_USB`, `3.3V`).
  * 🔴 **Layer 4 (Bottom):** Secondary signal traces and high-current power breakouts.
* 🛠️ **Connectors & Assembly:**
  * 🔲 LQFP-144 package (0.5 mm pitch) for STM32H745, QFN-88 packages for 2x GL3523 hubs, and M.2 Key M 2242/2280 socket for Hailo-8.
  * 🔌 Dual Hirose DF40 mezzanine connectors for Compute Module 5.
  * 📌 40-pin, 2.54 mm-pitch STACKING header for STACK A bus connection (base of the physical Robot Controller Board stack) - `docs/PINOUT_STACKA_CONNECTOR.TXT`.
  * 🔌 8x USB 3.0 Type-A (or Hirose industrial latching) connectors for robot cameras.

---

## 12. 🦾 ROBOT CONTROLLER BOARDS & URTC TOOL HEAD (DISTRIBUTED TIER)

Each of the up to 8 slave modules on STACK A (section 6) is a **Robot
Controller Board**: one per robot, driving that robot's own 6 axes
(STEP/DIR/ENABLE), reading its endstops, and forwarding its own tool head's
traffic one hop further over a *second* CAN connection to a **URTC** board
(Universal Robot Tool Controller - see the sibling `URTC` repository) mounted
in the robot's head, optionally with its own expansion board.

```mermaid
flowchart LR
    MCU["STM32H745<br/>FDCAN1 (STACK A)"] --> RCB["<b>Robot Controller Board</b><br/>x1 per robot, up to 8<br/>6x STEP/DIR/EN, endstops"]
    RCB -- CAN --> URTC["<b>URTC Tool Head</b><br/>+ optional expansion board"]
    URTC -- CAN --> RCB
```

* 🎛️ **MCU:** STMicroelectronics **STM32G474RET6** (Cortex-M4 @ 170 MHz,
  LQFP-64, 512 KB flash), using 2 of its 3 onboard FDCAN peripherals - one as
  the FDCAN uplink to the STM32H745, one as the CAN downlink to its own URTC
  head. See `docs/architecture.md` §1.
* 🔢 **Addressing:** `BOARD_ID[2:0]` - a local 3-position DIP switch on each
  board, manually set to 0-7 at install time, gives every board its own
  FDCAN1 slot base ID - not derived from the physical stack position or the
  STACK A connector (every board is the same interchangeable PCB). See
  `docs/PINOUT_STM32G474_ROBOT_CONTROLLER.TXT` §1c.
* 🧵 **RTOS:** **FreeRTOS** (its bootloader stays bare-metal - no scheduler
  needed to receive/verify/jump). Firmware skeleton: `src/mcu_stm32g474/`.
* 📡 **CAN-OTA firmware updates, 4 tiers deep:** the STM32H745 itself (over
  its existing SPI link to the CM5), this board, its URTC Tool Head
  (STM32F303CCT6), and - only when installed - that head's own Advanced
  Expansion Board (STM32F303CBT6, `expansion_board_type` 3 or 4, see URTC's
  own `docs/EXPANSION.TXT`) can all be flashed and diagnosed from
  HYDRA-UMC-STUDIO's Flasher/Tester with no JTAG/SWD probe and no USB-CAN
  dongle. Full addressing scheme, the relay tunnel that reaches the last two
  tiers without any new protocol design, and current implementation status:
  `docs/architecture.md`.

See `docs/architecture.md` for the complete tiered architecture (this section
is a summary), including what's confirmed hardware fact vs. still a proposed
design pending implementation. Section 8 of that document also tracks the
current bootloaders' known, accepted security limitations (no Read-Out
Protection yet, a shared anti-rollback bypass value, unauthenticated
readback) - deliberate pre-hardware gaps, not oversights.

---

## 📂 REPOSITORY DIRECTORY STRUCTURE

```text
HYDRA-UMC/
├── .vscode/                    # Recommended extensions + build tasks - see "Development Environment" below
├── docs/
│   ├── datasheets/             # Datasheets of parts used across every board in this repo
│   ├── architecture.md         # The 4-tier system architecture (start here)
│   ├── COMPILE_STM32G474.TXT   # Robot Controller Board firmware build reference
│   ├── COMPILE_STM32H745.TXT   # Kinematic Brain firmware build reference (dual-core)
│   ├── PINOUT_STM32H745_KINEMATIC_BRAIN.TXT    # Kinematic Brain full pin allocation
│   ├── PINOUT_STM32G474_ROBOT_CONTROLLER.TXT   # Robot Controller Board full pin allocation
│   ├── PINOUT_CM5_CARRIER.TXT                  # CM5 host subsystem signal routing
│   ├── PINOUT_STACKA_CONNECTOR.TXT             # Shared 40-pin STACK A stacking connector
│   ├── CANBUS_STM32H745.TXT                    # Kinematic Brain wire-level protocol (SPI1/mailbox/FDCAN1-master)
│   ├── CANBUS_STM32G474.TXT                    # Robot Controller Board wire-level protocol (FDCAN1-slave/FDCAN2)
│   └── HYDRA-UMC_*.txt/TXT     # Older docs - several superseded, see each file's own banner
├── hardware/
│   ├── PCB/
│   │   ├── kinematic_brain_stm32h745/          # Main motherboard - no schematic yet, see its own README
│   │   └── robot_controller_board_stm32g474/   # Per-robot board - no schematic yet, see its own README
│   └── gerbers/                # Manufacturing output files (empty until a board is laid out)
├── src/                         # Same layout convention as the sibling URTC repo: src/ is SOURCE
│   ├── cm5_host/                # Linux userspace apps that run ON TOP of os/'s own image
│   │   ├── hmi_qt6/             # Qt6 kiosk shell wrapping HYDRA-UMC-STUDIO's own dashboard
│   │   ├── ai_inference/        # Hailo-8 TAPPAS / YOLOv8 pipeline
│   │   ├── video_streamer/      # Multi-camera RTSP/WebRTC server (MediaMTX)
│   │   └── ipc_driver/          # CM5 <-> STM32H745 SPI link (userspace)
│   ├── mcu_stm32h745/           # Kinematic Brain firmware (Tier 0) - dual-core
│   │   ├── CM7/                 # Motion engine, hardware timers (+ its own boot/)
│   │   ├── CM4/                 # FDCAN drivers, sensor filtering (+ its own boot/)
│   │   └── Common/              # CM7<->CM4 shared-memory IPC mailbox (ipc_mailbox.h) - implemented, used by both cores' bootloaders
│   └── mcu_stm32g474/           # Robot Controller Board firmware (Tier 1) - single-core, + its own boot/
├── os/                          # CM5 OS image - base OS choice, systemd units, first-boot provisioning
├── images/                      # README banner + icon + splashscreen (SVG)
├── build_firmware.sh            # Builds every MCU firmware target above from a clean checkout (Linux/Mac)
├── build_firmware.bat           # Same build, Windows (see "Building the Firmware" below)
├── generate_manifest.py         # Regenerates firmware/firmware_manifest.json (versions/CRC32) after a full build
├── firmware/                    # Committed build output (.bin/.hex/.elf + manifest) - NOT gitignored, same convention as URTC's own output folder, see "Building the Firmware" below
├── README.md                    # This file
└── README_spa.md / README_ita.md / README_fra.md / README_deu.md    # <- translations
```

See `docs/architecture.md` for what each tier actually does and how they
connect; every folder above with its own `README.md` has more detail than
this top-level summary.

## 🛠️ DEVELOPMENT ENVIRONMENT

What this project's own development machines actually have installed and
verified working (`build_firmware.sh`/`build_firmware.bat`, `g474`/`h745`/
default targets, 0 errors) - not a theoretical list:

* 🔧 **ARM GNU Toolchain** (`arm-none-eabi-gcc` 10.3+) - compiles every MCU
  firmware target. No STM32CubeIDE/CubeMX project files are used or
  required to build - `build_firmware.sh`/`build_firmware.bat` fetches ST's
  own HAL/CMSIS sources fresh from their official GitHub repos and drives
  the compiler directly, same philosophy the sibling `URTC` repo's own
  `build_firmware.sh`/`build_firmware.bat` already established.
* 🧩 **VS Code + extensions** (`.vscode/extensions.json` lists all of
  these): [STM32 VS Code Extension](https://marketplace.visualstudio.com/items?itemName=stmicroelectronics.stm32-vscode-extension)
  (project/build/debug integration), **Cortex-Debug** (SWD/JTAG debugging -
  independent of `build_firmware.sh`, useful once real hardware exists),
  **CMake Tools** (for `src/cm5_host/hmi_qt6/`'s own CMake project),
  **C/C++** (IntelliSense across every firmware/host source file),
  **Python** (`ai_inference/` pipeline scripts), **Hex Editor** (inspecting
  `.bin` firmware output), **YAML** (`video_streamer/`'s own MediaMTX
  config). Open the repo, accept the recommended-extensions prompt, and use
  **Terminal → Run Task** for the pre-wired build tasks
  (`.vscode/tasks.json`).
* 🗂️ **git** - both for this repo itself and for `build_firmware.sh`'s own
  pinned-tag vendoring of ST's HAL/CMSIS packages (cached under `build/`,
  gitignored, re-fetched on `--clean`).

## 🏗️ BUILDING THE FIRMWARE

**Linux/Mac:**
```bash
./build_firmware.sh          # builds every MCU target (Robot Controller Board + Kinematic Brain, both cores)
./build_firmware.sh g474     # Robot Controller Board only
./build_firmware.sh h745     # Kinematic Brain only (both cores)
./build_firmware.sh --clean  # wipe the vendored HAL/CMSIS cache first
```

**Windows:**
```bat
build_firmware.bat          :: builds every MCU target (Robot Controller Board + Kinematic Brain, both cores)
build_firmware.bat g474     :: Robot Controller Board only
build_firmware.bat h745     :: Kinematic Brain only (both cores)
build_firmware.bat --clean  :: wipe the vendored HAL/CMSIS cache first
```

`build_firmware.bat` is the same build as `build_firmware.sh` translated to
batch (same steps, same pinned HAL/CMSIS versions, same pass/warn/fail
reporting) - run end to end on a real Windows machine with the [Arm GNU
Toolchain](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads)
installed and `arm-none-eabi-gcc` on `PATH`: every HAL module, both
bootloaders, and every application compiled and linked clean, and
`firmware_manifest.json` regenerated with CRC32s matching the Linux/Mac
build's own output. Requires the same tools as the Linux/Mac script: the Arm
GNU Toolchain, `git` (to fetch ST's own HAL/CMSIS sources), and `python` for
the manifest step.

**Manual build (either OS, without the script):** the script automates
exactly the steps in `docs/COMPILE_STM32G474.TXT` and
`docs/COMPILE_STM32H745.TXT` - fetch the pinned HAL/CMSIS/FreeRTOS sources
listed at the top of `build_firmware.sh`/`build_firmware.bat`, compile each
target's HAL modules and startup/system files with `arm-none-eabi-gcc`
(flags/module lists are listed in that same script), then link each
bootloader and application against its own linker script (`*.ld`, next to
its source) with `arm-none-eabi-gcc`/`-Wl,--gc-sections` and convert with
`arm-none-eabi-objcopy` to `.bin`/`.hex`. Those two `docs/COMPILE_*.TXT`
files are the authoritative, step-by-step reference if you'd rather not run
either script - the scripts exist to automate them, not replace them as the
source of truth.

Output lands in `firmware/`, which is committed and pushed to this
repo (same convention as URTC's own `firmware/` output folder) so that
HYDRA-UMC-STUDIO's GitHub-download feature can actually find real
`.bin` files there via `firmware_manifest.json` - it is NOT gitignored.
See `docs/COMPILE_STM32G474.TXT` and `docs/COMPILE_STM32H745.TXT` for
exactly what each step does and why - and each firmware folder's own
`README.md` for current status. The **bootloaders** for all 3 targets
(G474, H745 CM7, H745 CM4) are real, working CAN-OTA/SPI-OTA
implementations (CRC32 + HMAC-SHA256 verify-into-backup-before-copy-to-main,
same anti-bricking discipline as URTC's own bootloader) - compiling clean
end to end, not yet verified against real hardware. The **applications**
are still verified-compiling FreeRTOS GPIO-toggle smoke tests, not yet the
real motion/vision/relay firmware. See `docs/architecture.md` (especially
section 6's status table and section 8's known, accepted security
limitations) for exactly what's real vs. still open.

## 🔗 Related Projects

This project is part of a larger robotics ecosystem by the same author (JuanenRac / Electro Hobby 3D). Worth knowing about, since a request might actually be about one of these rather than this repository:

**HYDRA-UMC platform** — the multi-robot micro-factory cell
- **HYDRA-UMC** *(this repository)* — the motherboard itself: Raspberry Pi CM5 host + dual-core STM32H745 real-time co-processor, orchestrating up to 8 distributed robot arms over CAN-OTA/SPI-OTA. Own hardware + firmware, GPL-3.0/CERN-OHL-S v2/CC BY-SA 4.0.
- **[HYDRA-UMC STUDIO](https://github.com/JuanenRac/HYDRA-UMC-STUDIO)** — web-based control dashboard for HYDRA-UMC: multi-robot 3D visualization, kinematics/trajectory recording, CAN-OTA flashing and testing for the whole platform. React + Vite + Three.js.
- **[HYDRA-UMC-ANDROID-CONTROL](https://github.com/JuanenRac/HYDRA-UMC-ANDROID-CONTROL)** — Android control app for HYDRA-UMC over Wi-Fi/Bluetooth. Real, working app - full remote-control feature set, JWT auth, encrypted credential storage.
- **[HYDRA-UMC-IOS-CONTROL](https://github.com/JuanenRac/HYDRA-UMC-IOS-CONTROL)** — iOS/iPadOS control app for HYDRA-UMC over Wi-Fi, built in Flutter (cross-platform, verifiable on Windows without a Mac; final `.ipa` packaging still needs Xcode). Real, working app - same feature set as the Android app.
- **[HYDRA-UMC-SUITE](https://github.com/JuanenRac/HYDRA-UMC-SUITE)** — desktop (Python/PySide6) swarm command center: multi-controller network discovery, live bidirectional sync, real 3D robot viewport, Photoshop-style dockable workspace. Real and working, not a placeholder.
- **[HYDRA-UMC-EDITOR-URDF](https://github.com/JuanenRac/HYDRA-UMC-EDITOR-URDF)** — desktop (Python/PySide6) graphical URDF creator/editor for this project's own model catalog: pulls source files from GitHub or a local folder, validates DOF feasibility, edits color/scale/kinematics with a live 3D preview, and pushes the finished result to a running STUDIO server. Real and working, not a placeholder.
- **[HYDRA-UMC-DSI](https://github.com/JuanenRac/HYDRA-UMC-DSI)** — planned: a native touch UI for HYDRA-UMC's own 7" DSI touchscreen (1280×800) on the Compute Module 5, controlling this same server directly from the board. Not started yet.

**URTC platform** — the tool head controller every HYDRA-UMC robot arm carries
- **[URTC](https://github.com/JuanenRac/URTC)** — Universal Robot Tool Controller: STM32F303-based CAN bus tool head controller, 25 fully-implemented tool profiles, CAN-OTA firmware update.
- **[URTC Flasher](https://github.com/JuanenRac/URTC-FLASHER)** — desktop CAN-OTA + full-chip SWD/JTAG flashing tool for URTC boards (Windows/Linux).
- **[URTC Tester](https://github.com/JuanenRac/URTC-TESTER)** — desktop live CAN-bus diagnostic tool for URTC boards, one panel per tool profile (Windows/Linux).
- **[URTC Web Studio](https://github.com/JuanenRac/URTC-WEB-STUDIO)** — browser-based alternative to the 2 desktop tools above (Web Serial API + SLCAN), no local install needed.

## 👤 Author

**JuanenRac** (Electro Hobby 3D)
📧 electrohobby3d@gmail.com
📺 youtube.com/@electrohobby3d

## 📜 License and Copyright Notices

HYDRA-UMC is (c) 2026 JuanenRac (Electro Hobby 3D). This notice must be included in any distributions of this project or derivative works.

Because this project consists of several different types of content, individual parts are made available under different licenses - each suited to what it actually covers, rather than forcing one license to fit everything:

1. The **firmware** located at `./firmware` (application and CAN bootloader alike) is available under the **GNU General Public License v3.0 (GPL-3.0)**. Full text at https://www.gnu.org/licenses/gpl-3.0.html.

2. The **hardware designs** (Eagle schematic/board files, gerbers, and the 3D-printable parts under `./hardware` and `./3D`) are available under the **CERN Open Hardware Licence v2 - Strongly Reciprocal (CERN-OHL-S v2)**. Full text at https://cern-ohl.web.cern.ch/.

3. The **documentation** (this README, the service manual, and the reference files under `./docs` is available under **Creative Commons Attribution-ShareAlike 4.0 International (CC BY-SA 4.0)**. Full text at https://creativecommons.org/licenses/by-sa/4.0/.

If you build on this project, keep the licensing split in mind: code changes to the firmware or the flashing tool should stay GPL-3.0, hardware modifications should stay CERN-OHL-S, and documentation derivatives should stay CC BY-SA - each with attribution back to this project.
