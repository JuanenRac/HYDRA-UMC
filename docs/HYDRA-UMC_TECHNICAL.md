> ⚠️ **SUPERSEDED / OUTDATED - DO NOT USE FOR NEW WORK.** This document describes an
> earlier board revision (STM32H757BIT6, LQFP-208, 12x onboard TMC5160A drivers
> wired directly to robots, ESP32-C3 Wi-Fi, USB2514B hub, LAN8720A Ethernet PHY,
> external SDRAM/QSPI flash). The CURRENT architecture (STM32H745ZIT6, LQFP-144,
> a single FDCAN1 bus to up to 8 external Robot Controller Boards - no onboard
> motor drivers) is documented in `README.md` and `docs/architecture.md`. Kept
> here for historical reference only. See `docs/architecture.md` for why the
> design changed and what's confirmed vs. still pending on the current revision.

# HYDRA Core Architecture Specifications

An industrial-grade, distributed multi-robot control system and high-performance HMI architecture designed for multi-axis cellular robotics, automated manufacturing, and complex toolhead orchestration. 

The **HYDRA Platform** separates high-level user interface rendering, motion planning, and cloud connectivity from real-time step generation, fieldbus protocol management, and power electronics actuation using a **Heterogeneous Host + Co-Processor Architecture**.

---

## 🛠 Architectural Overview

```
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
                                  |        STM32H757ZIT6 (REAL-TIME CO-PROCESSOR)         |
                                  |                 (LQFP-144 Package)                    |
                                  |                                                       |
                                  |  +------------------------+  +---------------------+  |
                                  |  | Cortex-M7 @ 480 MHz    |  | Cortex-M4 @ 240MHz  |  |
                                  |  | - S-Curve Kinematics   |  | - FDCAN1 / FDCAN2   |  |
                                  |  | - Hardware Timers      |  | - Sensor Filtering  |  |
                                  |  | - 6-Axis Local Stage   |  | - Inter-Core IPC    |  |
                                  |  +------------------------+  +---------------------+  |
                                  |                                                       |
                                  |  - 1 MB SRAM / 2 MB Dual-Bank Internal Flash          |
                                  |  - Dedicated SPI2 Interface to 64 KB FRAM             |
                                  +-------------+---------------------------+-------------+
                                                |                           |
                       +------------------------+                           +------------------------+
                       |                                                                             |
                       v                                                                             v
         +---------------------------+                                                 +---------------------------+
         |      FDCAN BUS #1         |                                                 |      FDCAN BUS #2         |
         |  (STACK A - 4x SLAVES)    |                                                 |  (STACK B - 4x SLAVES)    |
         +-------------+-------------+                                                 +-------------+-------------+
                       |                                                                             |
     +---------+-------+-------+---------+                                         +---------+-------+-------+---------+
     |         |               |         |                                         |         |               |         |
     v         v               v         v                                         v         v               v         v
+-----+   +-----+         +-----+   +-----+                                   +-----+   +-----+         +-----+   +-----+
| A1  |   | A2  |  ...    | A3  |   | A4  |                                   | B1  |   | B2  |  ...    | B3  |   | B4  |
+-----+   +-----+         +-----+   +-----+                                   +-----+   +-----+         +-----+   +-----+
```

---

## 1. Host Computing Subsystem (HMI & High-Level Processing)

* **Module:** Raspberry Pi Compute Module 5 (CM5)
* **Processor:** Broadcom BCM2712 Quad-Core ARM Cortex-A76 @ 2.4 GHz
* **Graphics Engine:** VideoCore VII GPU supporting OpenGL ES 3.1 and Vulkan 1.2
* **System Memory:** 2 GB / 4 GB LPDDR4X (Board-Down on CM5 module)
* **High-Speed Storage:** Integrated eMMC Flash + M.2 NVMe SSD support via PCIe Gen 2.0 x1 interface
* **Operating System:** Linux 64-bit (Raspberry Pi OS / Yocto Distribution patched with `PREEMPT_RT`)
* **Display Interface:** MIPI-DSI (2-lane / 4-lane) connected to high-resolution capacitive touch panel
* **Peripherals:** 
  * 1x Gigabit Ethernet (RJ45) for industrial networking / LAN
  * Integrated Wi-Fi 6 & Bluetooth 5.4
  * USB 3.0 Host ports for machine vision / inspection cameras

### Core Responsibilities:
1. **60 FPS HMI Execution:** Renders responsive UI screens (Bambu Lab-style workflow, 3D model orientation, realtime telemetry dashboards) built on Qt 6 / QML or Flutter.
2. **Motion Planning:** Parses incoming execution scripts / G-code and computes global velocity profiles, S-curve trajectory blending, and kinematic transformations.
3. **Computer Vision & Inspection:** Processes live video streams from USB/MIPI cameras for alignment, optical verification, and part detection.

---

## 2. Real-Time Co-Processing Subsystem

* **Microcontroller:** STMicroelectronics **STM32H757ZIT6**
* **Form Factor / Package:** LQFP-144 (0.5 mm pin pitch)
* **Architecture:** Dual-core Asymmetric Multiprocessing (AMP)
  * **Core 1:** ARM Cortex-M7 with FPU & DP-FPU running at **480 MHz**
  * **Core 2:** ARM Cortex-M4 with FPU running at **240 MHz**
* **On-Chip Memory Allocation:**
  * **2 MB** Dual-Bank Flash Memory (Zero-wait state execution via ART Accelerator)
  * **1 MB** Total System SRAM (512 KB AXI SRAM + 128 KB ITCM / 128 KB DTCM + SRAM1/SRAM2/SRAM3)

```
        +---------------------------------------------------------------+
        |                     STM32H757 INTERNAL SRAM                   |
        +-----------------------+-----------------------+---------------+
        |      ITCM (128 KB)    |     DTCM (128 KB)     |  AXI (512 KB) |
        | Fast Exec Code (M7)   | Real-time Data (M7)   | Ring Buffers  |
        +-----------------------+-----------------------+---------------+
```

### Core Responsibilities:
* **Cortex-M7 (480 MHz):** 
  * Generates hardware-timed pulse streams (`STEP`/`DIR`) for local 6-axis stage.
  * Calculates real-time S-curve motion profile segments and closed-loop control laws.
  * Executes PID loops for high-power thermal channels.
* **Cortex-M4 (240 MHz):** 
  * Handles frame packing/unpacking and interrupt servicing for dual FDCAN peripherals.
  * Processes analog sensor input streams (oversampled NTC thermistor channels).
  * Manages zero-copy shared memory queues for inter-core communications via Hardware Semaphore (`HSEM`).

---

## 3. Distributed Fieldbus Communication (FDCAN Architecture)

The HYDRA baseboard acts as a dual-master controller for 8 individual slave robotic modules distributed across two independent physical CAN networks.

* **Controller Peripherals:** 2x Native Hardware FDCAN Controllers (`FDCAN1`, `FDCAN2`) built directly into the STM32H757.
* **Physical Layer Transceivers:** 2x High-Speed CAN FD Transceivers (e.g., Texas Instruments `TCAN1044AVD` or NXP `TJA1443`).
* **Topology:**
  * **STACK A:** `FDCAN1` -> Slave Modules A1, A2, A3, A4.
  * **STACK B:** `FDCAN2` -> Slave Modules B1, B2, B3, B4.

```
                  +-----------------------------------+
                  |      STM32H757 FDCAN CONTROLLERS  |
                  +-----------------+-----------------+
                                    |
            +-----------------------+-----------------------+
            |                                               |
            v                                               v
     +--------------+                                +--------------+
     |   FDCAN 1    |                                |   FDCAN 2    |
     +------+-------+                                +------+-------+
            |                                               |
            v                                               v
    [TCAN1044 Transceiver]                          [TCAN1044 Transceiver]
            |                                               |
            v                                               v
   +------------------+                            +------------------+
   |  STACK A BUS     |                            |  STACK B BUS     |
   | (Robots A1 - A4) |                            | (Robots B1 - B4) |
   +------------------+                            +------------------+
```

### Protocol Specifications:
* **Nominal Bitrate (Arbitration):** 1 Mbps
* **Data Bitrate (Payload):** 5 Mbps to 8 Mbps (64-byte payload frames)
* **Bus Isolation:** Dedicated Ground return and power rails per stack connector.
* **Bus Health Monitoring:** Auto-bus-off recovery mechanisms managed directly by the Cortex-M4 core.

---

## 4. Ultra-Fast Non-Volatile Memory (SPI FRAM)

To ensure zero data loss during power disruptions, the baseboard incorporates a Ferroelectric Random Access Memory (FRAM) chip.

* **Memory IC:** Cypress/Infineon `FM25V05-G` or Fujitsu `MB85RS64` (64 KB / 512 Kb capacity)
* **Bus Interface:** SPI2 dedicated channel running up to 40 MHz.
* **Performance:** Infinite read/write endurance (10^14 write cycles) with nanosecond write latencies.

### Anti-Power Loss Sequence (PVD Protection):
1. The **Power Voltage Detector (PVD)** hardware inside the STM32H757 continuously monitors the primary 3.3V rail.
2. If input voltage drops below threshold (e.g., 2.9V), the PVD triggers an immediate NMI (Non-Maskable Interrupt).
3. The Cortex-M7 halts non-essential execution and dumps active encoder coordinates, axis state vectors, and production counters into the FRAM in under **5 microseconds** using direct register writes before supply collapse.

---

## 5. Baseboard Native Motion & Auxiliary Actuation

Beyond managing the distributed slave robots via CAN FD, the baseboard directly controls local cell peripherals and a Cartesian positioning stage:

### Motion Control Outputs
* **Supported Axes:** 6-Axis Cartesian Stage (`X`, `Y`, `Z`, `A`, `B`, `C`).
* **Interface Signals:** Differential / 3.3V CMOS (`STEP`, `DIR`, `ENABLE`).
* **Timer Infrastructure:** Driven by Advanced-Control Timers (`TIM1`, `TIM8`) and General-Purpose Timers (`TIM2`-`TIM5`) utilizing Direct Memory Access (DMA) for pulse generation without CPU stalling.

### Power & Fluidic Actuation
* **Low-Side Switching:** 16x Industrial N-Channel MOSFET outputs with flyback diode protection.
  * **8x Channels:** Electropneumatic Valves (5V/24V actuation).
  * **8x Channels:** High-flow Vacuum Pumps / Venturi Pick-and-Place generators.
* **Thermal Management:**
  * 1x High-Current Gate Driver output for Heated Bed / Reflow Element (PWM controlled, 24V bus).
  * 4x Precision NTC Thermistor analog inputs configured in Wheatstone bridge / voltage divider networks, sampled by 16-bit hardware-oversampled ADCs (`ADC1`, `ADC2`).

---

## 6. Power Distribution & Regulation Architecture

The motherboard operates off a single industrial 24V DC input supply bus.

```
                          +-------------------------+
                          |   MAIN INPUT: +24V DC   |
                          +------------+------------+
                                       |
                +----------------------+----------------------+
                |                                             |
                v                                             v
  +---------------------------+                 +---------------------------+
  |  High-Efficiency Buck     |                 |  24V High-Current Bus     |
  |  Regulator (24V -> 5V)    |                 |  - Heated Bed             |
  |  Rating: 5A Continuous    |                 |  - 16x Solenoids & Pumps  |
  +-------------+-------------+                 |  - STACK A/B Power Rails  |
                |                               +---------------------------+
        +-------+-------+
        |               |
        v               v
  +-----------+   +---------------------------+
  | CM5 Host  |   | LDO / Buck Regulator      |
  | & Display |   | (5V -> 3.3V)              |
  +-----------+   | Rating: 1.5A Continuous   |
                  +-------------+-------------+
                                |
                                v
                  +---------------------------+
                  |  3.3V Ultra-Low-Noise Bus |
                  |  - STM32H757 VDD / VDDA   |
                  |  - FRAM Memory            |
                  |  - FDCAN Transceivers     |
                  +---------------------------+
```

* **Primary DC Bus:** 24V DC ±10%
* **5V Power Domain:** Step-down synchronous switching regulator (e.g., TI `LM2596S-5.0` or `TPS54560`) capable of supplying **5A continuous** to power the CM5 module, touch display backlights, and USB expansion buses.
* **3.3V Power Domain:** Secondary low-noise LDO / Regulator supplying **1.5A continuous** dedicated exclusively to the STM32H757 micro-controller core, digital I/O, SPI FRAM, and CAN FD transceivers to prevent ground bounce and inductive noise corruption.

---

## 7. Inter-Processor Communication (IPC) Protocol

Communication between the Compute Module 5 (Linux Host) and the STM32H757 (Real-Time Controller) uses a hardware-assisted, zero-copy SPI protocol.

* **Physical Transport:** Full-Duplex SPI1 operating in Slave Mode on the STM32H757 and Master Mode on the CM5 (Clock frequencies: 25 MHz to 50 MHz).
* **Handshake Signals:**
  * `SPI1_SCK`, `SPI1_MISO`, `SPI1_MOSI`, `SPI1_CS`
  * `HYDRA_DATA_READY` (Host-bound Interrupt Line driven by STM32 GPIO)

### Frame Transaction Lifecycle:
1. When the Cortex-M4 aggregates telemetry from STACK A and STACK B, it constructs a fixed-size 128-byte packet in shared AXI SRAM.
2. The STM32 asserts the `HYDRA_DATA_READY` interrupt line high.
3. The CM5 Linux kernel receives the GPIO edge interrupt and initiates a high-speed SPI DMA transaction.
4. The packet is processed on the CM5 host within a user-space C++/Qt worker thread without CPU polling overhead.

---

## 8. PCB Hardware & Manufacturing Requirements

* **Form Factor:** Single Monolithic Motherboard.
* **Layer Count:** Standard **4-Layer Stackup**:
  * **Layer 1 (Top):** Component placement, high-frequency signals, differential pairs.
  * **Layer 2 (Inner 1):** Solid, uninterrupted Ground Plane (`GND`).
  * **Layer 3 (Inner 2):** Split Power Planes (`24V`, `5V`, `3.3V`).
  * **Layer 4 (Bottom):** Secondary signal traces and power breakouts.
* **Assembly Requirements:**
  * Hand-solderable / Reflowable **LQFP-144** pitch (0.5 mm pin spacing) for the main MCU.
  * Dual **Hirose DF40** high-density board-to-board mezzanine connectors for the Compute Module 5.
  * Dual 2x20 dual-row shrouded pin headers (2.54 mm pitch) with heavy-copper power routing for STACK A and STACK B interconnections.

---

## 📂 Repository Directory Structure

```text
hydra-platform/
├── docs/
│   ├── architecture.md
│   └── pinout_stm32h757_lqfp144.csv
├── hardware/
│   ├── schematics/             # Eagle SCH files
│   ├── board_layout/           # Eagle BRD files
│   └── gerbers/                # Manufacturing output files
├── firmware/
│   ├── cm5_host/               # Linux system services, Qt UI, Klipper integration
│   │   ├── hmi_qt6/
│   │   └── ipc_driver/
│   └── mcu_stm32h757/          # STM32CubeIDE dual-core firmware project
│       ├── CM7/                # Motion engine, hardware timers, PID loops
│       ├── CM4/                # FDCAN stack drivers, telemetry parser
│       └── Common/             # Shared memory structure definitions
└── README.md
```