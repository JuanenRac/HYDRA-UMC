# 🚀 HYDRA-UMC TECHNICAL SPECIFICATION
### 🤖 The Ultimate Dual-Core Micro-Factory & Multi-Robot Controller Platform (V2.2 - Single FDCAN Bus)

---

## 1. 🛠️ PROJECT OVERVIEW & THE MICRO-FACTORY ECOSYSTEM

**HYDRA-UMC** (Universal Machines Controller) is an industrial-grade, distributed control platform and high-performance HMI architecture designed for multi-axis cellular robotics, micro-factories, automated manufacturing, and complex toolhead orchestration. 

Built on a **Heterogeneous Host + Real-Time Co-Processor Architecture**, HYDRA-UMC decouples high-level user interface rendering, computer vision, and cloud connectivity from real-time step generation, fieldbus management, and power electronics actuation.

```text
                                  +-------------------------------------------------------+
                                  |            COMPUTE MODULE 5 (HOST / CEREBRO)          |
                                  | - Broadcom BCM2712 Quad Cortex-A76 @ 2.4 GHz          |
                                  | - VideoCore VII GPU (OpenGL ES 3.1 / Vulkan 1.2)      |
                                  | - Linux OS with PREEMPT_RT Patchset                   |
                                  | - High-FPS Touch UI (Qt6 / Flutter) via MIPI-DSI      |
                                  | - Trajectory Planning, G-code Parsing, Vision AI      |
                                  +---------------------------+---------------------------+
                                                              |
                                           High-Speed SPI Bus + DMA + IRQ Pin
                                                              |
                                  +---------------------------v---------------------------+
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
* 📐 **Local 6-Axis Stage:** Direct step/dir/enable pulse generation for 6 local axes (X, Y, Z, A, B, C) driving Cartesian positioning systems, indexers, or local gantries.
* 🎯 **JuanenPNP & JuanenCNC Integration:** Directly compatible with Pick-and-Place systems (LumenPNP hardware structures) and CNC units equipped with 10W optical laser modules for PCB prototyping and SMD placement.
* 👁️ **Computer Vision & Inspection:** Direct USB host interfaces allow multi-camera streams for real-time OpenCV pick-and-place optical alignment, part verification, and quality inspection.
* ⚡ **Actuation Matrix & Thermal Management:** Controls 16 industrial low-side MOSFET channels (8 electropneumatic valves + 8 vacuum pumps/venturi generators) and high-current bed drivers for SMD reflow soldering or 3D printing beds.
* 🚜 **JuanenBOT Mobile Platforms:** Scalable communication architecture capable of interfacing with heavy-duty 48V 4-wheeled transport platforms (50x50x50 cm frames with omnidirectional/mecanum wheels for 100 kg payloads).

---

## 2. 🖥️ HOST COMPUTING SUBSYSTEM (HMI & HIGH-LEVEL)

* 🧩 **Module:** Raspberry Pi Compute Module 5 (CM5)
* ⚙️ **Processor:** Broadcom BCM2712 Quad-Core ARM Cortex-A76 @ 2.4 GHz
* 🎮 **Graphics Engine:** VideoCore VII GPU (OpenGL ES 3.1, Vulkan 1.2)
* 💾 **System Memory:** 2 GB / 4 GB LPDDR4X (Integrated on CM5)
* 💽 **High-Speed Storage:** Integrated eMMC Flash + NVMe M.2 SSD support via PCIe Gen 2.0 x1
* 🐧 **Operating System:** Linux 64-bit (Raspberry Pi OS / Yocto patched with `PREEMPT_RT`)
* 📺 **Display Interface:** MIPI-DSI (2-lane / 4-lane) connected to high-resolution capacitive touch panel (Bambu Lab style UI at 60 FPS)
* 🌐 **Connectivity Suite:**
  * 🌐 1x Gigabit Ethernet (RJ45) for industrial LAN / WebSockets / MQTT
  * 📶 Wi-Fi 6 & Bluetooth 5.4
  * 🔌 USB 3.0 / 2.0 Host ports for machine vision cameras

---

## 3. ⚡ REAL-TIME CO-PROCESSING SUBSYSTEM

* 🎛️ **Microcontroller:** STMicroelectronics **STM32H745ZIT6** (Cost-optimized dual-core MCU)
* 📦 **Package:** LQFP-144 (0.5 mm pin pitch)
* 🧠 **Architecture:** Dual-Core Asymmetric Multiprocessing (AMP)
  * 🚀 **Core 1 (Cortex-M7 @ 480 MHz):** Real-time motion engine, hardware pulse generation, S-curve kinematic velocity profiles, PID control loops.
  * 📡 **Core 2 (Cortex-M4 @ 240 MHz):** FDCAN protocol management, analog sensor filtering, safety interlocks, and inter-core IPC handling.
* 💾 **Internal Memory Architecture:**
  * 💾 **2 MB** Dual-Bank Internal Flash
  * 🧠 **1 MB** Total Internal SRAM (512 KB AXI SRAM + 128 KB ITCM / 128 KB DTCM + SRAM1/SRAM2/SRAM3)

---

## 4. 📡 DISTRIBUTED FIELDBUS COMMUNICATION (SINGLE FDCAN)

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

## 5. 💾 ULTRA-FAST NON-VOLATILE MEMORY (SPI FRAM)

To guarantee zero data loss and instant state recovery during emergency power disruptions:

* 🧪 **Memory IC:** Cypress/Infineon `FM25V05-G` / Fujitsu `MB85RS64` (64 KB SPI FRAM)
* ⚡ **Bus Interface:** Dedicated SPI2 bus up to 40 MHz.
* ♾️ **Durability:** Infinite endurance ($10^{14}$ cycles) with nanosecond write latencies.
* 🛡️ **Anti-Power Loss Sequence (PVD):** The internal Power Voltage Detector (PVD) monitors the 3.3V rail. Upon voltage drop detection, a Non-Maskable Interrupt (NMI) dumps encoder vectors, active state machines, and coordinates to FRAM in under **5 microseconds** before supply shutdown.

---

## 6. 🦾 LOCAL MOTION, ACTUATION & SENSOR SUITE

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

## 7. 🔌 POWER DISTRIBUTION & REGULATION

The board operates from a single industrial **24V DC** input supply bus:

* ⚡ **Main DC Input:** 24V DC $\pm 10\%$
* 🔋 **5V Power Domain:** Step-down synchronous buck regulator supplying **5A continuous** for the CM5 module, touchscreen display backlight, and USB expansion.
* 🎛️ **3.3V Power Domain:** Secondary low-noise regulator supplying **1.5A continuous** dedicated exclusively to the STM32H745 MCU, SPI FRAM, and CAN FD transceiver to prevent inductive noise coupling.

---

## 8. 🔄 INTER-PROCESSOR COMMUNICATION (IPC)

Communication between CM5 (Host) and STM32H745 (Co-Processor) utilizes a hardware-assisted, zero-copy SPI link:

* 🔗 **Physical Transport:** Full-Duplex SPI1 running up to 50 MHz in Slave Mode on STM32 and Master Mode on CM5.
* 🤝 **Handshake Line:** `HYDRA_DATA_READY` GPIO line.
* ⚡ **Execution Flow:** The Cortex-M4 prepares a 128-byte telemetry frame in shared AXI SRAM, asserts `HYDRA_DATA_READY`, and the CM5 fetches the packet via high-speed SPI DMA without polling overhead.

---

## 9. 🎛️ 4-LAYER PCB HARDWARE SPECIFICATIONS

* 📐 **Form Factor:** Monolithic Industrial Motherboard.
* 🥞 **Layer Stackup (4-Layer):**
  * 🟢 **Layer 1 (Top):** Component placement, high-frequency signals, differential pairs.
  * 🛡️ **Layer 2 (Inner 1):** Continuous solid Ground Plane (`GND`).
  * ⚡ **Layer 3 (Inner 2):** Split Power Planes (`24V`, `5V`, `3.3V`).
  * 🔴 **Layer 4 (Bottom):** Secondary signal traces and high-current power breakouts.
* 🛠️ **Connectors & Assembly:**
  * 🔲 LQFP-144 package (0.5 mm pitch) for straightforward reflow and inspection.
  * 🔌 Dual Hirose DF40 mezzanine connectors for Compute Module 5.
  * 📌 Heavy-copper shrouded connector (2.54 mm pitch) for STACK A bus connection.

---

## 📂 REPOSITORY DIRECTORY STRUCTURE

```text
hydra-platform/
├── docs/
│   ├── architecture.md
│   └── pinout_stm32h745_lqfp144.csv
├── hardware/
│   ├── schematics/             # Eagle SCH files
│   ├── board_layout/           # Eagle BRD files
│   └── gerbers/                # Manufacturing output files
├── firmware/
│   ├── cm5_host/               # Linux system services, Qt UI, IPC driver
│   │   ├── hmi_qt6/
│   │   └── ipc_driver/
│   └── mcu_stm32h745/          # STM32CubeIDE dual-core project
│       ├── CM7/                # Motion engine, hardware timers, PID
│       ├── CM4/                # FDCAN drivers, sensor filtering
│       └── Common/             # Shared memory structure definitions
└── README.md
```

## 👤 Author

**JuanenRac** (Electro Hobby 3D)
📧 electrohobby3d@gmail.com
📺 [youtube.com/@electrohobby3d](https://youtube.com/@electrohobby3d)

## 📜 License and Copyright Notices

HYDRA-UMC is (c) 2026 JuanenRac (Electro Hobby 3D). This notice must be included in any distributions of this project or derivative works.

Because this project consists of several different types of content, individual parts are made available under different licenses - each suited to what it actually covers, rather than forcing one license to fit everything:

1. The **firmware** located at `./firmware` (application and CAN bootloader alike) is available under the **GNU General Public License v3.0 (GPL-3.0)**. Full text at https://www.gnu.org/licenses/gpl-3.0.html.

2. The **PC tools** at `./tools` are source code, licensed the same way and for the same reason as the firmware: **GNU General Public License v3.0 (GPL-3.0)**. This covers `urtc_flasher.py` and `urtc_tester.py` themselves and any `.exe`/binary built from either via their respective `build_exe.bat`/`build_exe.sh` — distributing a compiled tool means distributing something GPL-3.0 covers, same as distributing a compiled firmware `.bin` does.

3. The **hardware designs** (Eagle schematic/board files, gerbers, and the 3D-printable parts under `./hardware` and `./3D`) are available under the **CERN Open Hardware Licence v2 - Strongly Reciprocal (CERN-OHL-S v2)**. Full text at https://cern-ohl.web.cern.ch/.

4. The **documentation** (this README, the service manual, and the reference files under `./docs`, including `./tools/flasher/V1.0/README.md` and `./tools/tester/V1.0/README.md`) is available under **Creative Commons Attribution-ShareAlike 4.0 International (CC BY-SA 4.0)**. Full text at https://creativecommons.org/licenses/by-sa/4.0/.

If you build on this project, keep the licensing split in mind: code changes to the firmware or the flashing tool should stay GPL-3.0, hardware modifications should stay CERN-OHL-S, and documentation derivatives should stay CC BY-SA - each with attribution back to this project.
