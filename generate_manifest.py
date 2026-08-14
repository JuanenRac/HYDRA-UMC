#!/usr/bin/env python3
# =============================================================================
# generate_manifest.py - Regenerate firmware_out/firmware_manifest.json
#
# PROJECT: HYDRA-UMC
# AUTHOR: JuanenRac (Electro Hobby 3D) - electrohobby3d@gmail.com
# LICENSE: GPL-3.0 (same as the firmware this indexes - see LICENSE at repo root)
#
# Mirrors the sibling URTC repo's own generate_manifest.py byte-for-byte in
# approach (same schema shape, same "read real #defines, compute a real
# CRC32 over the real .bin" philosophy) - HYDRA-UMC-STUDIO's own
# src/lib/canOta.ts fetchGithubFirmwareReleases() reads either repo's
# manifest through one shared code path, so keeping the schema identical
# isn't a style preference, it's what makes that possible. Reads every
# component's own real version straight from its own bootloader_common.h
# (never hardcoded here) and computes a fresh CRC32 over each real .bin
# file in firmware_out/ - run this any time firmware_out/ changes and the
# manifest needs to catch up, not just after a full rebuild.
# build_firmware.sh calls this same script as its own last step.
#
# NOTE ON WHERE THIS LIVES: URTC commits its build OUTPUT straight into a
# `firmware/` folder. HYDRA-UMC's own `firmware/` is SOURCE (mcu_stm32g474/,
# mcu_stm32h745/) - build output goes to `firmware_out/` instead, which is
# gitignored by design (see .gitignore). This script still writes a real
# manifest there on every build; whether firmware_out/ itself ever gets
# committed+pushed (so HYDRA-UMC-STUDIO's own GitHub-download feature has
# something to actually find) is the project owner's own call, not decided
# by this script.
#
# Usage: python3 generate_manifest.py [repo_root]
#   repo_root defaults to this script's own parent directory.
# =============================================================================
import sys
import os
import re
import glob
import zlib
import json
from datetime import datetime, timezone


def read_define(path, name):
    with open(path) as f:
        text = f.read()
    m = re.search(rf"#define\s+{name}\s+(\S+)", text)
    if not m:
        raise SystemExit(
            f"generate_manifest.py: could not find #define {name} in {path} - "
            f"manifest NOT written, check this script's own regex against that "
            f"file's real current content before trusting anything else here."
        )
    return m.group(1).rstrip("UL")


def find_versioned_bin(fw_out, prefix):
    """build_firmware.sh embeds each binary's own version in its filename
    (e.g. HYDRA_RCB_BOOTLOADER_v1.0.0.bin) - glob for it rather than
    reconstructing the exact name here, so this script can't silently drift
    out of sync with that script's own naming convention."""
    matches = sorted(glob.glob(os.path.join(fw_out, f"{prefix}_v*.bin")))
    if not matches:
        raise SystemExit(
            f"generate_manifest.py: no {prefix}_v*.bin found in {fw_out} - "
            f"run a full build first (build_firmware.sh with no target "
            f"argument, or 'all'), manifest NOT written."
        )
    # Most-recently-built if more than one lingers (stale ones should be
    # cleaned up separately - this script indexes, it doesn't clean up).
    return max(matches, key=os.path.getmtime)


def bin_info(path):
    with open(path, "rb") as f:
        data = f.read()
    crc = zlib.crc32(data) & 0xFFFFFFFF
    return {"filename": os.path.basename(path), "size_bytes": len(data), "crc32": f"0x{crc:08X}"}


def file_info(bin_path, ext):
    path = os.path.splitext(bin_path)[0] + "." + ext
    if not os.path.exists(path):
        return None
    return {"filename": os.path.basename(path), "size_bytes": os.path.getsize(path)}


def component(fw_out, prefix, display_name, chip, hw_id, ver, flash_address, flash_region_max_bytes):
    bin_path = find_versioned_bin(fw_out, prefix)
    return {
        "display_name": display_name,
        "chip": chip,
        "hardware_id": hw_id,
        "version": ver,
        "version_string": ".".join(str(v) for v in ver.values()),
        "version_code": ver.get("major", 0) * 10000 + ver.get("minor", 0) * 100 + ver.get("patch", 0),
        "flash_address": flash_address,
        "flash_region_max_bytes": flash_region_max_bytes,
        "files": {
            "bin": bin_info(bin_path),
            "hex": file_info(bin_path, "hex"),
            "elf": file_info(bin_path, "elf"),
        },
    }


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))
    fw_out = os.path.join(root, "firmware_out")

    g4_h = os.path.join(root, "firmware", "mcu_stm32g474", "boot", "bootloader_common.h")
    g4_boot_ver = {k: int(read_define(g4_h, f"BOOTLOADER_VERSION_{k.upper()}")) for k in ("major", "minor", "patch")}
    g4_app_ver = {k: int(read_define(g4_h, f"FIRMWARE_VERSION_{k.upper()}")) for k in ("major", "minor")}
    g4_hwid = read_define(g4_h, "THIS_HARDWARE_ID")

    cm7_h = os.path.join(root, "firmware", "mcu_stm32h745", "CM7", "boot", "bootloader_common.h")
    cm7_boot_ver = {k: int(read_define(cm7_h, f"BOOTLOADER_VERSION_{k.upper()}")) for k in ("major", "minor", "patch")}
    cm7_app_ver = {k: int(read_define(cm7_h, f"FIRMWARE_VERSION_{k.upper()}")) for k in ("major", "minor")}
    cm7_hwid = read_define(cm7_h, "THIS_HARDWARE_ID")

    cm4_h = os.path.join(root, "firmware", "mcu_stm32h745", "CM4", "boot", "bootloader_common.h")
    cm4_boot_ver = {k: int(read_define(cm4_h, f"BOOTLOADER_VERSION_{k.upper()}")) for k in ("major", "minor", "patch")}
    cm4_app_ver = {k: int(read_define(cm4_h, f"FIRMWARE_VERSION_{k.upper()}")) for k in ("major", "minor")}
    cm4_hwid = read_define(cm4_h, "THIS_HARDWARE_ID")

    manifest = {
        "schema_version": 1,
        "generated_at": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "project": {
            "name": "HYDRA-UMC",
            "full_name": "Hybrid Dual-core Robotics Automation - Universal Micro-factory Controller",
            "repository": "https://github.com/JuanenRac/HYDRA-UMC",
            "author": "JuanenRac (Electro Hobby 3D)",
        },
        "notes": [
            "This indexes 6 INDEPENDENT components across 2 chips (STM32G474RET6 "
            "Robot Controller Board; STM32H745ZIT6 Kinematic Brain, 2 cores each "
            "with their own bootloader+application) - version_code is only "
            "meaningful compared against an EARLIER manifest's own value for that "
            "exact same component key. Comparing version_code ACROSS different "
            "component keys means nothing.",
            "version_code = major*10000 + minor*100 + patch (patch=0 for the 3 "
            "application components, which don't carry one) - bigger is newer, "
            "safe for simple numeric comparison. Don't compare version_string "
            "alphabetically.",
            "crc32 is computed over the real .bin file exactly as shipped here "
            "(standard CRC-32/ISO-HDLC), the same variant this project's own "
            "bootloaders compute internally during a real update (see each "
            "firmware/*/boot/bootloader_flash.c's own CRC32_Update/"
            "CRC32_Finalize) and the same variant HYDRA-UMC-STUDIO's own "
            "src/lib/canOta.ts checks a download against before offering to "
            "flash it - a mismatch means the file changed since this manifest "
            "was generated.",
            "hardware_id is what the CAN-OTA/SPI-OTA QUERY_VERSION command "
            "reports live from a real connected board (docs/CANBUS_STM32G474.TXT, "
            "docs/CANBUS_STM32H745.TXT) - cross-check it against that live "
            "answer before flashing anything, not just this manifest's own "
            "filename-to-chip mapping.",
            "flash_region_max_bytes is the maximum this component could ever be "
            "(this chip's own sector/page-aligned slot size), not this specific "
            "build's own actual size (see files.bin.size_bytes for that).",
            "Regenerated by generate_manifest.py, called automatically as the "
            "last step of a full build_firmware.sh run - hand-editing this file "
            "is pointless, it'll be overwritten the next time either script (or "
            "this one directly) runs.",
            "firmware_out/ is gitignored in this repo (see .gitignore) - this "
            "manifest existing locally does NOT mean it's reachable from GitHub. "
            "HYDRA-UMC-STUDIO's own GitHub-download feature only finds anything "
            "here once firmware_out/ is actually committed and pushed - a "
            "project-owner decision, not something this script or the build "
            "makes for you.",
        ],
        "components": {
            "g474_bootloader": component(
                fw_out, "HYDRA_RCB_BOOTLOADER", "Robot Controller Board Bootloader",
                "STM32G474RET6", g4_hwid, g4_boot_ver, "0x08000000", 32 * 1024,
            ),
            "g474_application": component(
                fw_out, "HYDRA_RCB_APP", "Robot Controller Board Application",
                "STM32G474RET6", g4_hwid, g4_app_ver, "0x08009000", 236 * 1024,
            ),
            "h745_cm7_bootloader": component(
                fw_out, "HYDRA_KB_CM7_BOOTLOADER", "Kinematic Brain Bootloader (CM7)",
                "STM32H745ZIT6 (CM7)", cm7_hwid, cm7_boot_ver, "0x08000000", 128 * 1024,
            ),
            "h745_cm7_application": component(
                fw_out, "HYDRA_KB_CM7_APP", "Kinematic Brain Application (CM7)",
                "STM32H745ZIT6 (CM7)", cm7_hwid, cm7_app_ver, "0x08040000", 384 * 1024,
            ),
            "h745_cm4_bootloader": component(
                fw_out, "HYDRA_KB_CM4_BOOTLOADER", "Kinematic Brain Bootloader (CM4)",
                "STM32H745ZIT6 (CM4)", cm4_hwid, cm4_boot_ver, "0x08100000", 128 * 1024,
            ),
            "h745_cm4_application": component(
                fw_out, "HYDRA_KB_CM4_APP", "Kinematic Brain Application (CM4)",
                "STM32H745ZIT6 (CM4)", cm4_hwid, cm4_app_ver, "0x08140000", 384 * 1024,
            ),
        },
    }

    out_path = os.path.join(fw_out, "firmware_manifest.json")
    with open(out_path, "w") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    print(f"firmware_manifest.json regenerated at {out_path}")
    for key, comp in manifest["components"].items():
        print(f"  {key}: v{comp['version_string']}  {comp['files']['bin']['filename']}  crc32={comp['files']['bin']['crc32']}")


if __name__ == "__main__":
    main()
