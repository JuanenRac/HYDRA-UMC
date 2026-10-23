#!/usr/bin/env python3
# =============================================================================
# bump_version.py - increment ONE component's own version macros in its
# bootloader_common.h, in place, before that component gets compiled.
#
# PROJECT: HYDRA-UMC
# AUTHOR: JuanenRac (Electro Hobby 3D) - electrohobby3d@gmail.com
# LICENSE: GPL-3.0-or-later (same as the firmware this versions - see LICENSE at repo root)
#
# ECOSYSTEM-WIDE VERSIONING POLICY, as implemented for THIS repo: same
# scheme as the sibling URTC repo (see URTC's own bump_version.py and
# CHANGELOG.md) - ALL components are incremental, never bumped by hand.
# HYDRA-UMC has SIX of them - the 3 bootloaders AND the 3 applications
# each bump their own PATCH by exactly 1 on every real build that
# produces a new binary for that component. That's what this script does.
# It is run explicitly (bump_firmware_versions.py), never by the build
# scripts, and BEFORE that component's own .c files (which #include this
# exact header) reach the compiler -
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

# From this version on a header with a _BUILD macro counts it as a fourth component.
FOUR_PART_FROM = (0, 8, 0)


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
    # A header may carry a fourth component (<prefix>_BUILD). It stays at 0 while
    # the version is three-part and only counts from FOUR_PART_FROM on, exactly
    # like the shared version utility. This script looks only at the header it
    # was given - never at the project manifest.
    has_build = re.search(rf"#define\s+{prefix}_BUILD\s+\d+", text) is not None
    build = get("BUILD") if has_build else None
    four_part = has_build and (major, minor, patch) >= FOUR_PART_FROM

    # The odometer carry rule itself - see this file's own header comment.
    if four_part:
        build += 1
        if build > 9:
            build = 0
            patch += 1
    else:
        patch += 1
    if patch > 9:
        patch = 0
        minor += 1
    if minor > 9:
        minor = 0
        major += 1
    if has_build and not four_part and (major, minor, patch) >= FOUR_PART_FROM:
        build = 0
        four_part = True

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
    if four_part:
        set_macro("BUILD", build)

    with open(path, "w") as f:
        f.write(text)

    print(f"{major}.{minor}.{patch}" + (f".{build}" if four_part else ""))
    return 0


def _delegate_to_shared_utility():
    """Hand the bump over to the shared utility once the version has four parts.

    Returns its exit code, or None when this step is an ordinary three-part one.
    """
    import importlib.util
    import json as _json
    from pathlib import Path as _Path

    here = _Path(__file__).resolve().parent
    root = here if (here / "bump_manifest_version.py").is_file() else here.parent
    utility, manifest = root / "bump_manifest_version.py", root / "hydra-umc.project.json"
    if not utility.is_file() or not manifest.is_file():
        return None
    spec = importlib.util.spec_from_file_location("_shared_bump", utility)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    version = _json.loads(manifest.read_text(encoding="utf-8")).get("version", "")
    if not hasattr(module, "FOUR_PART_FROM") or module.next_version(version).count(".") != 3:
        return None
    return module.main()


if __name__ == "__main__":
    _shared_exit = _delegate_to_shared_utility()
    if _shared_exit is not None:
        raise SystemExit(_shared_exit)


if __name__ == "__main__":
    sys.exit(main())
