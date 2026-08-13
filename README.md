# 🚀 HYDRA-UMC TECHNICAL SPECIFICATION
### 🤖 The Ultimate Dual-Core Micro-Factory & Multi-Robot Controller Platform (V1.0 - PCIe Hailo-8 AI Accelerator & Dual USB 3.0 Hubs)

---

## 1. 🛠️ PROJECT OVERVIEW & THE MICRO-FACTORY ECOSYSTEM

**HYDRA-UMC** (Universal Multi-axis Controller) is an industrial-grade, distributed control platform and high-performance HMI architecture designed for multi-axis cellular robotics, micro-factories, automated manufacturing, and complex toolhead orchestration. 

Built on a **Heterogeneous Host + Real-Time Co-Processor Architecture**, HYDRA-UMC decouples high-level user interface rendering, computer vision, AI inference, and cloud connectivity from real-time step generation, fieldbus management, and power electronics actuation.

```text
                                  +-------------------------------------------------------+
                                  |            COMPUTE MODULE 5 (HOST / CEREBRO)          |
                                  | - Broadcom BCM2712 Quad Cortex-A76 @ 2.4 GHz          |
                                  | - VideoCore VII GPU (OpenGL ES 3.1 / Vulkan 1.2)      |
                                  | - RP1 Dual USB 3.0 Host Controllers (2x 5 Gbps)       |
                                  | - Linux OS with PREEMPT_RT Patchset                   |
                                  | - High-FPS Touch UI (Qt6 / Flutter) via MIPI-DSI      |
                                  | - Trajectory Planning, G-code Parsing & Vision AI     |
                                  +--------+------------------+------------------+--------+
                                           |                  |                  |
                    PCIe Gen 3.0 x1 Bus    |                  |                  |
                     (Up to 8 Gbps)        |                  |                  |
                                           |        USB3 Ch 1 |        USB3 Ch 2 |
                                           v                  v                  v
               +-----------------------------+   +------------+----+   +------------+----+
               | HAILO-8 M.2 AI ACCELERATOR  |   | GL3523 HUB #1   |   | GL3523 HUB #2   |
               | (26 TOPS Neural Coproc)     |   +------------+----+   +------------+----+
               +-----------------------------+                |                  |
                                                  4x USB3 Ports          4x USB3 Ports
                                                    (Cam 1-4)              (Cam 5-8)
                                                              |                  |
                                                              +--------+---------+
                                                                       |
                                                   High-Speed SPI Bus + DMA + IRQ Pin
                                                                       |
                                  +------------------------------------v------------------+
                                  |        STM32H745ZIT6 (REAL-TIME CO-PROCESSOR)         |
                                  |                 (LQFP-144 Package)                    |
                                  |                                                       |
                                  |  +------------------------+  +---------------------+  |
                                  |  | Cortex-M7 @ 480 MHz    |  | Cortex-M4 @ 240MHz  |  |
                                  |  | - S-Curve Kinematics   |  | - FDCAN1 Controller |  |
                                  |  | - Hardware Timers      |  | - Sensor Filtering  |  |
                                  |  | - 6-Axis Local Stage   |  | - Inter-Core IPC    |  |
                                  |  +------------------------+  +---------------------+  |
                                  |                                                       |
                                  |  - 1 MB SRAM / 2 MB Dual-Bank Internal Flash          |
                                  |  - Dedicated SPI2 Interface to 64 KB FRAM             |
                                  +---------------------------+---------------------------+
                                                              |
                                                    FDCAN BUS #1 (FDCAN1)
                                                (STACK A - Up to 8x SLAVES)
                                                              |
     +---------+-------+-------+---------+-------+------------+------------+
     |         |               |         |       |            |            |
     v         v               v         v       v            v            v
+-----+   +-----+         +-----+   +-----+   +-----+      +-----+      +-----+
| A1  |   | A2  |  ...    | A3  |   | A4  |   | A5  | ...  | A7  |      | A8  |
+-----+   +-----+         +-----+   +-----+   +-----+      +-----+      +-----+
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

---

## 3. 🧠 PCIE AI ACCELERATOR SUBSYSTEM (HAILO-8 NPU)

* 🔌 **Physical Interface:** Onboard M.2 Key M socket (2242 / 2280 form factor) connected directly to the CM5 PCIe Gen 2.0 / 3.0 x1 bus.
* 🚀 **NPU Engine:** Hailo-8 Industrial AI Processor delivering **26 TOPS** (Tera Operations Per Second) at sub-5W power consumption.
* ⚡ **Software Integration:** Official Hailo RT software suite integrated with Raspberry Pi OS, executing GStreamer pipelines and OpenCV for zero-CPU-overhead neural inference.

---

## 4. 📷 DUAL USB 3.0 VISION SUBSYSTEM (8x CAMERA PORTS)

* 🎛️ **Hub Controllers:** 2x Genesys Logic `GL3523` USB 3.0 / SuperSpeed Hub ICs integrated directly on the motherboard.
* 🔀 **Topology & Distribution:**
  * 🅰️ **Hub #1 (`GL3523-A`):** Connected to RP1 USB3 Channel #1 (5 Gbps). Feeds USB Ports 1 to 4 (Cameras A1-A4).
  * 🅱️ **Hub #2 (`GL3523-B`):** Connected to RP1 USB3 Channel #2 (5 Gbps). Feeds USB Ports 5 to 8 (Cameras A5-A8).
* 🛡️ **Power Switch & Circuit Protection:** Individual USB VBUS protection via high-side current-limiting power switches (`TPS2065` / `SY6280`) configured for 500 mA - 1 A with fault reporting.
* ⚡ **High-Current VBUS Rail:** Powered by a dedicated 24V to 5V Step-Down Regulator (5V @ 6A continuous).

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

---

## 6. 📡 DISTRIBUTED FIELDBUS COMMUNICATION (SINGLE FDCAN)

The motherboard acts as a master controller for up to 8 individual slave robotic modules distributed across a single physical CAN FD network:

* 🔌 **Hardware Peripheral:** 1x Native Hardware FDCAN Controller (`FDCAN1`) built directly into the STM32H745.
* ⚡ **Physical Layer Transceiver:** 1x High-Speed CAN FD Transceiver (e.g., TI `TCAN1044AVD` / NXP `TJA1443`).
* 🔀 **Bus Topology:**
  * 🅰️ **STACK A (`FDCAN1`):** Serves Slave Modules A1 through A8.
* ⏱️ **Protocol Specs:** 1 Mbps Arbitration Bitrate, 5 Mbps to 8 Mbps Data Payload Bitrate (64-byte payload frames). Auto-bus-off recovery managed by Cortex-M4.

```text
                  +-----------------------------------+
                  |     STM32H745 FDCAN1 CONTROLLER   |
                  +-----------------+-----------------+
                                    |
                                    v
                         [TCAN1044 Transceiver]
                                    |
                                    v
                           +------------------+
                           |  STACK A BUS     |
                           | (Robots A1 - A8) |
                           +------------------+
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
* 🎯 **Supported Axes:** 6-Axis Cartesian Stage (`X`, `Y`, `Z`, `A`, `B`, `C`).
* ⚡ **Signals:** 3.3V CMOS / Differential (`STEP`, `DIR`, `ENABLE`).
* ⏱️ **Timers:** Advanced-Control Timers (`TIM1`, `TIM8`) and General Timers (`TIM2`-`TIM5`) with DMA pulse generation.

### 🔌 Power & Fluidic Actuators
* 🔀 **16x Low-Side Switching Channels:** Industrial N-Channel MOSFET outputs with flyback protection.
  * 💨 **8x Channels:** Electropneumatic Valves (5V/24V actuation).
  * 🧲 **8x Channels:** Vacuum Pumps / Venturi Pick-and-Place generators.
* 🌡️ **Thermal Management:**
  * 🔥 1x High-Current Gate Driver output for Heated Bed / Reflow Element (PWM controlled, 24V bus).
  * 🌡️ 4x Precision NTC Thermistor analog inputs sampled by 16-bit hardware-oversampled ADCs (`ADC1`, `ADC2`).

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
  * 📌 Heavy-copper shrouded connector (2.54 mm pitch) for STACK A bus connection.
  * 🔌 8x USB 3.0 Type-A (or Hirose industrial latching) connectors for robot cameras.

---

## 12. 🦾 ROBOT CONTROLLER BOARDS & URTC TOOL HEAD (DISTRIBUTED TIER)

Each of the up to 8 slave modules on STACK A (section 6) is a **Robot
Controller Board**: one per robot, driving that robot's own 6 axes
(STEP/DIR/ENABLE), reading its endstops, and forwarding its own tool head's
traffic one hop further over a *second* CAN connection to a **URTC** board
(Universal Robot Tool Controller - see the sibling `URTC` repository) mounted
in the robot's head, optionally with its own expansion board.

```text
   STM32H745 FDCAN1 (STACK A)
             |
             v
   +-------------------+        CAN         +-------------------+
   | Robot Controller  |-------------------->|   URTC Tool Head  |
   | Board (x1 per     |<--------------------|   (+ optional      |
   | robot, up to 8)   |                     |   expansion board) |
   +-------------------+                     +-------------------+
   6x STEP/DIR/EN, endstops
```

* 🎛️ **MCU:** STMicroelectronics **STM32G474RET6** (Cortex-M4 @ 170 MHz,
  LQFP-64, 512 KB flash), using 2 of its 3 onboard FDCAN peripherals - one as
  the FDCAN uplink to the STM32H745, one as the CAN downlink to its own URTC
  head. See `docs/architecture.md` §1/§4.
* 📡 **CAN-OTA firmware updates:** both this board and its URTC head are
  flashed over CAN only - no JTAG/SWD probe, no USB-CAN dongle. The
  HYDRA-UMC-STUDIO dashboard (running on the CM5) reaches either one through
  the full SPI → FDCAN1 → (relay) → CAN chain. Full addressing scheme and
  protocol reuse from URTC's own proven CAN bootloader: `docs/architecture.md`.

See `docs/architecture.md` for the complete tiered architecture (this section
is a summary), including what's confirmed hardware fact vs. still a proposed
design pending implementation.

---

## 📂 REPOSITORY DIRECTORY STRUCTURE

```text
hydra-platform/
├── docs/
│   ├── datasheets/             # Datasheets of parts project
│   ├── architecture.md
│   └── pinout_stm32h745_lqfp144.csv
├── hardware/
│   ├── PCB/                    # Eagle SCH & BRD files (MCU, CM5, 2x GL3523, M.2 Hailo-8, Power)
│   └── gerbers/                # Manufacturing output files
├── firmware/
│   ├── cm5_host/               # Linux system services, Qt UI, IPC driver, RTSP streamer, Hailo-8 pipeline
│   │   ├── hmi_qt6/            # 
│   │   ├── ai_inference/       # Hailo-8 TAPPAS / YOLOv8 pipeline
│   │   ├── video_streamer/     # Multi-camera RTSP/WebRTC server
│   │   └── ipc_driver/         #
│   └── mcu_stm32h745/          # STM32CubeIDE dual-core project
│       ├── CM7/                # Motion engine, hardware timers, PID
│       ├── CM4/                # FDCAN drivers, sensor filtering
│       └── Common/             # Shared memory structure definitions
└── README.md
```

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
