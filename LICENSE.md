# License and Copyright Notice

HYDRA-UMC is (c) 2026 JuanenRac (Electro Hobby 3D).

This notice must be included in any distributions of this project or derivative works.

Because this project consists of several different types of content, individual parts are made available under different licenses, each suited to what it actually covers:

1. The firmware located at `./firmware` (application and bootloader alike, all 3 MCU targets: STM32G474, STM32H745 CM7, STM32H745 CM4) and every script and tool of the repository are available under the **GNU General Public License v3.0 or later (GPL-3.0-or-later)**. The full GPL-3.0 text is the `LICENSE` file; the "or later" choice is stated in the notice at the top of each source file.
   Full text: https://www.gnu.org/licenses/gpl-3.0.html

2. The hardware designs (Eagle schematic/board files, gerbers, and the 3D-printable parts) located at `./hardware` and `./3D` are available under the **CERN Open Hardware Licence v2 - Strongly Reciprocal (CERN-OHL-S v2)**.
   Full text: https://cern-ohl.web.cern.ch/

3. The documentation (this README, the service manual, and the reference files under `./docs`) is available under **Creative Commons Attribution-ShareAlike 4.0 International (CC BY-SA 4.0)**.
   Full text: https://creativecommons.org/licenses/by-sa/4.0/

If you build on this project, keep the licensing split in mind: code changes should stay GPL-3.0-or-later, hardware modifications should stay CERN-OHL-S, and documentation derivatives should stay CC BY-SA, each with attribution back to this project and its author.

This repository covers the HYDRA-UMC motherboard's own firmware and hardware, plus the Robot Controller Board and Kinematic Brain firmware it distributes over CAN-OTA/SPI-OTA. The URTC Tool Head each robot arm ultimately carries is a separate project with its own license: https://github.com/JuanenRac/URTC. The web control dashboard (HYDRA-UMC STUDIO) is also a separate project with its own license: https://github.com/JuanenRac/HYDRA-UMC-STUDIO.

---

Author: JuanenRac (Electro Hobby 3D)
Contact: electrohobby3d@gmail.com
