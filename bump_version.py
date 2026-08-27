#!/usr/bin/env python3
# =============================================================================
# bump_version.py - increment ONE component's own version macros in its
# bootloader_common.h, in place, before that component gets compiled.
#
# PROJECT: HYDRA-UMC
# AUTHOR: JuanenRac (Electro Hobby 3D) - electrohobby3d@gmail.com
# LICENSE: GPL-3.0 (same as the firmware this versions - see LICENSE at repo root)
#
# ECOSYSTEM-WIDE VERSIONING POLICY, as implemented for THIS repo: same
# scheme as the sibling URTC repo (see URTC's own bump_version.py and
# CHANGELOG.md) - ALL components are incremental, never bumped by hand.
# HYDRA-UMC has SIX of them - the 3 bootloaders AND the 3 applications
# each bump their own PATCH by exactly 1 on every real build that
# produces a new binary for that component, fully automatically. That's
# what this script does, and why build_firmware.sh/.bat call it as the very
# first step of compiling EACH of the 6 components, before that component's
# own .c files (which #include this exact header) ever reach the compiler -
# bootloader_protocol.c on all 3 chips reads these same macros at compile
# time (see ApplicationIsValid()'s first-boot-adoption path), so bumping
# AFTER compiling would ship a binary whose own reported version lags what
# its filename claims.
#
# "Odometer" carry rule (matches the general ecosystem-wide scheme; the
# owner's own worked example: 1.1.9 -> next build -> 1.2.0, NEVER 1.1.10):
#   patch += 1
#   if patch > 9: patch = 0; minor += 1
#   if minor > 9: minor = 0; major += 1
# (the second check runs even when the first one didn't fire this time, so a
# minor that was already sitting at 9 for some other reason still carries
# correctly - though in practice here it only ever fires right after the
# first carry, since nothing else in this project bumps minor directly).
#
# Usage: python3 bump_version.py <header_path> <MACRO_PREFIX>
#   header_path   path to the component's own bootloader_common.h
#   MACRO_PREFIX  "BOOTLOADER_VERSION" or "FIRMWARE_VERSION" - this script
#                 touches ONLY that macro family's own _MAJOR/_MINOR/_PATCH
#                 #defines, leaving the other family (which shares the same
#                 physical header file - each chip has ONE bootloader_common.h
#                 defining BOTH its bootloader's own version and the FIRMWARE_
#                 VERSION_* the app it hands off to reports) untouched.
#
# Prints the new "MAJOR.MINOR.PATCH" to stdout on success (nothing else, so
# a caller can capture it directly); exits non-zero with a message on stderr
# if the expected #defines aren't found - never writes a partial/inconsistent
# header.
# =============================================================================
import sys
import re


def main():
    if len(sys.argv) != 3:
        print("usage: bump_version.py <header_path> <MACRO_PREFIX>", file=sys.stderr)
        return 1
    path, prefix = sys.argv[1], sys.argv[2]

    with open(path) as f:
        text = f.read()

    def get(name):
        m = re.search(rf"#define\s+{prefix}_{name}\s+(\d+)", text)
        if not m:
            print(
                f"bump_version.py: could not find #define {prefix}_{name} in {path} "
                f"- header NOT modified, check this script's own regex against that "
                f"file's real current content before trusting anything else here.",
                file=sys.stderr,
            )
            sys.exit(1)
        return int(m.group(1))

    major, minor, patch = get("MAJOR"), get("MINOR"), get("PATCH")

    # The odometer carry rule itself - see this file's own header comment.
    patch += 1
    if patch > 9:
        patch = 0
        minor += 1
    if minor > 9:
        minor = 0
        major += 1

    def set_macro(name, value):
        nonlocal text
        new_text, n = re.subn(
            rf"(#define\s+{prefix}_{name}\s+)\d+",
            rf"\g<1>{value}",
            text,
            count=1,
        )
        if n != 1:
            print(
                f"bump_version.py: expected exactly 1 occurrence of #define "
                f"{prefix}_{name} in {path}, found {n} - header NOT modified.",
                file=sys.stderr,
            )
            sys.exit(1)
        text = new_text

    set_macro("MAJOR", major)
    set_macro("MINOR", minor)
    set_macro("PATCH", patch)

    with open(path, "w") as f:
        f.write(text)

    print(f"{major}.{minor}.{patch}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
