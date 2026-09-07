set -e
# HYDRA_UMC_SCRIPT_STANDARD_HEADER_BEGIN
# *****************************************************************************
# Project   : HYDRA-UMC
# Script    : build_firmware.sh
# Purpose   : Incremental firmware build and versioned artifact packaging workflow.
# Author    : JuanenRac (Electro Hobby 3D)
# Email     : electrohobby3d@gmail.com
# Copyright : (C) 2026 JuanenRac
# License   : GPL-3.0 - see LICENSE
# *****************************************************************************
# HYDRA_UMC_SCRIPT_STANDARD_HEADER_END
# HYDRA_UMC_SCRIPT_STANDARD_BANNER_BEGIN
printf '\n*******************************************************************************\n'
printf '%s\n' "* HYDRA-UMC - build_firmware.sh"
printf '%s\n' "* Mode      : INCREMENTAL BUILD"
printf '%s\n' "* Author    : JuanenRac (Electro Hobby 3D)"
printf '%s\n' "* Email     : electrohobby3d@gmail.com"
printf '%s\n' "* Copyright : (C) 2026 JuanenRac"
printf '%s\n' "* License   : GPL-3.0 - see LICENSE"
printf '%s\n' "* ------------------------------------------------------------------------- *"
printf '%s\n' "* 1. Increment the project version and synchronise its manifest."
printf '%s\n' "* 2. Run this project's declared build, verification and packaging commands."
printf '%s\n' "* 3. Report the result and keep an interactive terminal open."
printf '%s\n' "*******************************************************************************"
printf '\n'
# HYDRA_UMC_SCRIPT_STANDARD_BANNER_END
HYDRA_UMC_CI_MODE="${HYDRA_UMC_CI:-0}"
if [ "$HYDRA_UMC_CI_MODE" = "1" ]; then
    echo "HYDRA-UMC CI: version sources are read-only."
else
    # HYDRA_UMC_SCRIPT_STANDARD_VERSION_STEP
    printf '%s\n' "[1/3] Incrementing project version and synchronising its manifest..."
    # HYDRA_UMC_SCRIPT_STANDARD_VERSION_CAPTURE_BEFORE
    HYDRA_UMC_VERSION_BEFORE="$(python3 -c 'import json, pathlib, sys; print(json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))["version"])' "$(dirname "$0")/hydra-umc.project.json")"
    # The registry version is the G474 application version. Its component bump
    # occurs later, then --sync records that one authoritative bump.
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="$ROOT/build"
FIRMWARE_OUT="$ROOT/firmware"
if [ -t 0 ]; then
    trap 'echo ""; read -r -p "Press Enter to close this window..." _' EXIT
fi

# Pinned versions - see docs/COMPILE_STM32G474.TXT section 3 for why pinned
# rather than tracking each repo's own latest master.
G4_HAL_REPO="https://github.com/STMicroelectronics/stm32g4xx_hal_driver.git"
G4_HAL_TAG="v1.2.7"
G4_CMSIS_DEVICE_REPO="https://github.com/STMicroelectronics/cmsis_device_g4.git"
G4_CMSIS_DEVICE_TAG="v1.2.6"
CMSIS_CORE_REPO="https://github.com/STMicroelectronics/cmsis_core.git"
CMSIS_CORE_TAG="v5.9.0_20250520"
H7_HAL_REPO="https://github.com/STMicroelectronics/stm32h7xx_hal_driver.git"
H7_HAL_TAG="v1.11.6"
H7_CMSIS_DEVICE_REPO="https://github.com/STMicroelectronics/cmsis_device_h7.git"
H7_CMSIS_DEVICE_TAG="v1.10.7"
FREERTOS_REPO="https://github.com/FreeRTOS/FreeRTOS-Kernel.git"
FREERTOS_TAG="V11.3.0"

PASS=0; WARN=0; FAIL=0
pass() { echo "  OK   $1"; PASS=$((PASS+1)); }
warn() { echo "  WARN $1"; WARN=$((WARN+1)); }
fail() { echo "  FAIL $1"; FAIL=$((FAIL+1)); }
step() { echo ""; echo "=== $1 ==="; }

TARGET="${1:-all}"
if [ "$1" = "--clean" ]; then
    echo "Removing $BUILD ..."
    rm -rf "$BUILD"
    TARGET="${2:-all}"
fi

# -----------------------------------------------------------------------
step "1. Toolchain"
# -----------------------------------------------------------------------
if ! command -v arm-none-eabi-gcc >/dev/null 2>&1; then
    echo "arm-none-eabi-gcc not found - attempting install via apt..."
    if command -v apt >/dev/null 2>&1; then
        sudo apt update
        sudo apt install -y gcc-arm-none-eabi binutils-arm-none-eabi \
            libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib \
            libstdc++-arm-none-eabi-dev git
    else
        fail "no apt available and arm-none-eabi-gcc is missing - install the ARM GNU Toolchain manually, then re-run this script"
        echo ""; echo "$PASS passed, $WARN warnings, $FAIL failed"; exit 1
    fi
fi
if command -v arm-none-eabi-gcc >/dev/null 2>&1; then
    pass "arm-none-eabi-gcc found: $(arm-none-eabi-gcc --version | head -1)"
else
    fail "arm-none-eabi-gcc still not found after install attempt"
    echo ""; echo "$PASS passed, $WARN warnings, $FAIL failed"; exit 1
fi
for tool in arm-none-eabi-objcopy arm-none-eabi-size arm-none-eabi-nm; do
    if command -v $tool >/dev/null 2>&1; then
        pass "$tool found"
    else
        fail "$tool not found - the gcc-arm-none-eabi package should provide this; check your install"
    fi
done
if command -v git >/dev/null 2>&1; then
    pass "git found: $(git --version)"
else
    fail "git not found - needed to fetch ST's own HAL/CMSIS sources."
    echo ""; echo "$PASS passed, $WARN warnings, $FAIL failed"; exit 1
fi
if command -v python3 >/dev/null 2>&1; then
    pass "python3 found: $(python3 --version)"
else
    fail "python3 not found - required to run bump_version.py (all 6 components are incremental, see this script's own version-bump steps below) and generate_manifest.py."
    echo ""; echo "$PASS passed, $WARN warnings, $FAIL failed"; exit 1
fi

mkdir -p "$FIRMWARE_OUT"

# A firmware directory represents one build set, never an accumulation of
# differently-versioned binaries.  Keep non-generated material intact, but
# remove every artifact this script can publish before compiling the next set.
shopt -s nullglob
firmware_artifacts=(
    "$FIRMWARE_OUT"/HYDRA_*.bin
    "$FIRMWARE_OUT"/HYDRA_*.elf
    "$FIRMWARE_OUT"/HYDRA_*.hex
    "$FIRMWARE_OUT"/firmware_manifest.json
)
if ((${#firmware_artifacts[@]})); then
    echo ""
    echo "=== Firmware output cleanup ==="
    rm -f -- "${firmware_artifacts[@]}"
    pass "removed ${#firmware_artifacts[@]} generated firmware artifact(s) from firmware/"
fi
shopt -u nullglob

build_bin_hex() {
    local elf="$1"
    arm-none-eabi-objcopy -O binary "$elf" "${elf%.elf}.bin"
    arm-none-eabi-objcopy -O ihex "$elf" "${elf%.elf}.hex"
}

# Extracts a #define's integer value from a header - used below to embed
# each bootloader's own BOOTLOADER_VERSION_MAJOR/MINOR/PATCH (bootloader_
# common.h) and each application's own FIRMWARE_VERSION_MAJOR/MINOR into
# its output filename, so the version a schematic/BOM/HYDRA-UMC-STUDIO
# Flasher screen reports is read from the SAME source the firmware itself
# was built from - never hand-typed twice, never able to drift out of sync.
get_version_macro() {
    local header="$1" macro="$2"
    grep -oE "define[[:space:]]+${macro}[[:space:]]+[0-9]+" "$header" | grep -oE '[0-9]+$'
}

# Bumps ONE component's own version macro family (BOOTLOADER_VERSION or
# FIRMWARE_VERSION) in its bootloader_common.h in place, via bump_version.py
# (odometer carry rule: PATCH past 9 -> MINOR+1, PATCH resets to 0 - see
# that script's own header comment for the full reasoning). Called BEFORE
# each of the 6 components below gets compiled, so the just-bumped value is
# what actually ends up baked into that binary (bootloader_protocol.c reads
# these same macros at compile time) and in its output filename - never
# bumped after the fact. Per this repo's own versioning policy, ALL 6
# components (3 bootloaders + 3 applications) are incremental this way,
# unlike sibling repo URTC where only the bootloaders are (and there, bumped
# by hand, not automatically like here).
bump_version() {
    local header="$1" prefix="$2" label="$3"
    local newver
    if [ "$HYDRA_UMC_CI_MODE" = "1" ]; then
        newver="$(get_version_macro "$header" "${prefix}_MAJOR").$(get_version_macro "$header" "${prefix}_MINOR").$(get_version_macro "$header" "${prefix}_PATCH")"
        pass "$label version verified at v$newver (CI; source unchanged)"
        return
    fi
    if ! newver="$(python3 "$ROOT/bump_version.py" "$header" "$prefix")"; then
        fail "$label: version bump failed - see bump_version.py's own error above"
        echo ""; echo "$PASS passed, $WARN warnings, $FAIL failed"; exit 1
    fi
    pass "$label version bumped to v$newver ($header)"
}

# Compiles every .c file in a directory (non-recursive) - same helper URTC's
# own build_firmware.sh already uses for its own partitioned bootloader
# sources (src/F303-master/boot/, src/F303-slave/boot/). Every bootloader in
# this project is now split the same way (bootloader_main.c, bootloader_
# common.h, bootloader_crypto.c, bootloader_flash.c, bootloader_protocol.c),
# so this replaces what used to be a single hardcoded bootloader_main.c
# compile line per target.
compile_dir() {
    local srcdir="$1" outdir="$2" cflags="$3" extra_inc="$4"
    for f in "$srcdir"/*.c; do
        [ -e "$f" ] || continue
        arm-none-eabi-gcc $cflags -I"$srcdir" $extra_inc -x c -c "$f" -o "$outdir/$(basename "$f" .c).o" || return 1
    done
    return 0
}

# Runs a link command, filtering ld's own known-harmless notes out of the
# printed output, WITHOUT losing the linker's real exit code the way
# `cmd 2>&1 | grep -v "..." || true` used to - that pattern reports $? as
# grep's own exit status (1 whenever every line got filtered out, 0
# otherwise), then `|| true` masks even that, so a genuine link failure
# that still emitted a partial/corrupt .elf was only ever caught later, if
# at all (e.g. by build_bin_hex's own objcopy failing to find a valid ELF
# - not guaranteed for every possible failure mode). Using a temp file for
# stderr instead of a pipe means $? right after the command is the
# linker's own, unmodified.
link_filtered() {
    local errfile
    errfile="$(mktemp)"
    "$@" 2>"$errfile"
    local status=$?
    grep -v "not implemented\|note: the message\|in function \`_" "$errfile" >&2
    rm -f "$errfile"
    return $status
}

# -----------------------------------------------------------------------
step "2. FreeRTOS kernel sources (shared by every application target - see docs/architecture.md)"
# -----------------------------------------------------------------------
FREERTOS_VENDOR="$BUILD/vendor/freertos"
if [ ! -d "$FREERTOS_VENDOR" ]; then
    echo "Fetching FreeRTOS-Kernel ($FREERTOS_TAG)..."
    git -c safe.directory='*' clone --depth 1 --branch "$FREERTOS_TAG" -q "$FREERTOS_REPO" "$FREERTOS_VENDOR"
    pass "FreeRTOS-Kernel fetched"
else
    pass "FreeRTOS-Kernel already cached at build/vendor/freertos"
fi
FREERTOS_SRC="$FREERTOS_VENDOR"
FREERTOS_COMMON_SOURCES="tasks.c queue.c list.c timers.c event_groups.c"

# Compiles the FreeRTOS kernel + the given port + heap_4, into $4, using the
# given CFLAGS (which must already -I the target's own FreeRTOSConfig.h
# directory) - shared helper for all 3 application targets below. Bootloaders
# never call this (bare-metal, see this script's own header comment).
compile_freertos() {
    local cflags="$1" port_dir="$2" objdir="$3"
    mkdir -p "$objdir"
    for f in $FREERTOS_COMMON_SOURCES; do
        if [ ! -f "$objdir/$f.o" ] || [ "$FREERTOS_SRC/$f" -nt "$objdir/$f.o" ]; then
            arm-none-eabi-gcc $cflags -I"$FREERTOS_SRC/include" -I"$port_dir" -x c -c "$FREERTOS_SRC/$f" -o "$objdir/$f.o" || return 1
        fi
    done
    arm-none-eabi-gcc $cflags -I"$FREERTOS_SRC/include" -I"$port_dir" -x c -c "$port_dir/port.c" -o "$objdir/port.o" || return 1
    arm-none-eabi-gcc $cflags -I"$FREERTOS_SRC/include" -I"$port_dir" -x c -c "$FREERTOS_SRC/portable/MemMang/heap_4.c" -o "$objdir/heap_4.o" || return 1
    return 0
}

# =========================================================================
if [ "$TARGET" = "all" ] || [ "$TARGET" = "g474" ]; then
# =========================================================================
step "3. Robot Controller Board (STM32G474RET6) - ST HAL/CMSIS sources"
# -----------------------------------------------------------------------
G4="$BUILD/g474"
mkdir -p "$G4/vendor" "$G4/common/HAL_Include" "$G4/common/CMSIS_Include" "$G4/hal_src" "$G4/hal_obj" "$G4/app" "$G4/boot_obj" "$G4/app_obj" "$G4/freertos_obj"

if [ ! -d "$G4/vendor/hal" ]; then
    echo "Fetching STM32G4xx HAL driver ($G4_HAL_TAG)..."
    git -c safe.directory='*' clone --depth 1 --branch "$G4_HAL_TAG" -q "$G4_HAL_REPO" "$G4/vendor/hal"
    pass "HAL driver fetched"
else
    pass "HAL driver already cached at build/g474/vendor/hal"
fi

if [ ! -d "$G4/vendor/cmsis_device_g4" ]; then
    echo "Fetching CMSIS device headers for G4 ($G4_CMSIS_DEVICE_TAG)..."
    git -c safe.directory='*' clone --depth 1 --branch "$G4_CMSIS_DEVICE_TAG" -q "$G4_CMSIS_DEVICE_REPO" "$G4/vendor/cmsis_device_g4"
    pass "CMSIS device (G4) fetched"
else
    pass "CMSIS device (G4) already cached at build/g474/vendor/cmsis_device_g4"
fi

if [ ! -d "$G4/vendor/cmsis_core" ]; then
    echo "Fetching generic ARM CMSIS Core headers (Include/ only, sparse)..."
    git -c safe.directory='*' clone --depth 1 --filter=blob:none --no-checkout -q --branch "$CMSIS_CORE_TAG" "$CMSIS_CORE_REPO" "$G4/vendor/cmsis_core"
    git -C "$G4/vendor/cmsis_core" -c safe.directory='*' sparse-checkout init --no-cone
    echo "/Core/Include/**" > "$G4/vendor/cmsis_core/.git/info/sparse-checkout"
    git -C "$G4/vendor/cmsis_core" -c safe.directory='*' checkout -q
    pass "CMSIS core fetched (Include/ only)"
else
    pass "CMSIS core already cached at build/g474/vendor/cmsis_core"
fi

cp "$G4/vendor/hal/Inc/"*.h "$G4/common/HAL_Include/" 2>/dev/null || true
cp -r "$G4/vendor/hal/Inc/Legacy" "$G4/common/HAL_Include/" 2>/dev/null || true
cp "$G4/common/HAL_Include/stm32g4xx_hal_conf_template.h" "$G4/common/HAL_Include/stm32g4xx_hal_conf.h"
cp "$G4/vendor/hal/Src/"*.c "$G4/hal_src/"
cp "$G4/vendor/cmsis_device_g4/Include/"*.h "$G4/common/CMSIS_Include/"
cp -r "$G4/vendor/cmsis_core/Core/Include/"* "$G4/common/CMSIS_Include/"
if [ -f "$G4/common/CMSIS_Include/core_cm4.h" ] && [ -f "$G4/common/HAL_Include/stm32g4xx_hal.h" ]; then
    pass "HAL/CMSIS include tree assembled"
else
    fail "HAL/CMSIS include tree incomplete - check build/g474/common/ manually"
fi

# -----------------------------------------------------------------------
step "4. Robot Controller Board - common compiler flags and shared HAL objects"
# -----------------------------------------------------------------------
CFLAGS_G4="-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard -DSTM32G474xx -DUSE_HAL_DRIVER -I$G4/common/CMSIS_Include -I$G4/common/HAL_Include -O2 -Wall -ffunction-sections -fdata-sections"
LDCOMMON_G4="-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard -specs=nano.specs -specs=nosys.specs -Wl,--gc-sections"
# App CFLAGS additionally need this target's own FreeRTOSConfig.h directory
# plus FreeRTOS's own headers/port headers - the bootloader (bare-metal, no
# FreeRTOS) uses CFLAGS_G4 alone.
CFLAGS_G4_APP="$CFLAGS_G4 -I$ROOT/src/mcu_stm32g474 -I$FREERTOS_SRC/include -I$FREERTOS_SRC/portable/GCC/ARM_CM4F"

# HAL core + RCC + GPIO + Cortex + PWR + FLASH (app smoke test) plus FDCAN +
# IWDG (the real CAN-OTA bootloader, src/mcu_stm32g474/boot/) - extend
# further as real motion/timer application code lands.
HAL_MODULES_G4="stm32g4xx_hal stm32g4xx_hal_cortex stm32g4xx_hal_gpio stm32g4xx_hal_rcc stm32g4xx_hal_rcc_ex stm32g4xx_hal_pwr stm32g4xx_hal_pwr_ex stm32g4xx_hal_flash stm32g4xx_hal_flash_ex stm32g4xx_hal_exti stm32g4xx_hal_fdcan stm32g4xx_hal_iwdg"

HAL_OK=1
for f in $HAL_MODULES_G4; do
    if [ ! -f "$G4/hal_obj/$f.o" ] || [ "$G4/hal_src/$f.c" -nt "$G4/hal_obj/$f.o" ]; then
        arm-none-eabi-gcc $CFLAGS_G4 -x c -c "$G4/hal_src/$f.c" -o "$G4/hal_obj/$f.o" || HAL_OK=0
    fi
done
COUNT=$(ls "$G4/hal_obj"/*.o 2>/dev/null | wc -l)
if [ "$HAL_OK" = "1" ] && [ "$COUNT" -ge 1 ]; then
    pass "$COUNT HAL modules compiled"
else
    fail "one or more HAL modules failed to compile - see errors above"
    echo ""; echo "$PASS passed, $WARN warnings, $FAIL failed"; exit 1
fi

cp "$G4/vendor/cmsis_device_g4/Source/Templates/gcc/startup_stm32g474xx.s" "$G4/app/"
cp "$G4/vendor/cmsis_device_g4/Source/Templates/system_stm32g4xx.c" "$G4/app/"
arm-none-eabi-gcc $CFLAGS_G4 -x assembler-with-cpp -c "$G4/app/startup_stm32g474xx.s" -o "$G4/app/startup.o"
arm-none-eabi-gcc $CFLAGS_G4 -x c -c "$G4/app/system_stm32g4xx.c" -o "$G4/app/system_stm32g4xx.o"
pass "startup + system files compiled"

if compile_freertos "$CFLAGS_G4_APP" "$FREERTOS_SRC/portable/GCC/ARM_CM4F" "$G4/freertos_obj"; then
    pass "FreeRTOS kernel (ARM_CM4F port) compiled for Robot Controller Board"
else
    fail "FreeRTOS kernel failed to compile for Robot Controller Board - see errors above"
    echo ""; echo "$PASS passed, $WARN warnings, $FAIL failed"; exit 1
fi

# -----------------------------------------------------------------------
step "5. Robot Controller Board bootloader (src/mcu_stm32g474/boot/) - bare-metal CAN-OTA, no FreeRTOS"
# -----------------------------------------------------------------------
SRC="$ROOT/src/mcu_stm32g474/boot"
bump_version "$SRC/bootloader_common.h" BOOTLOADER_VERSION "Robot Controller Board bootloader"
rm -f "$G4/boot_obj"/*.o
if compile_dir "$SRC" "$G4/boot_obj" "$CFLAGS_G4" ""; then
    pass "bootloader_*.c compiled ($(ls "$SRC"/*.c | wc -l) files)"
else
    fail "Robot Controller Board bootloader failed to compile - see errors above"
    echo ""; echo "$PASS passed, $WARN warnings, $FAIL failed"; exit 1
fi
G4_BOOT_VER="v$(get_version_macro "$SRC/bootloader_common.h" BOOTLOADER_VERSION_MAJOR).$(get_version_macro "$SRC/bootloader_common.h" BOOTLOADER_VERSION_MINOR).$(get_version_macro "$SRC/bootloader_common.h" BOOTLOADER_VERSION_PATCH)"
G4_BOOT_NAME="HYDRA_RCB_BOOTLOADER_${G4_BOOT_VER}"
if ! link_filtered arm-none-eabi-gcc $LDCOMMON_G4 -T"$SRC/STM32G474RETx_BOOTLOADER.ld" \
    "$G4/app/startup.o" "$G4/app/system_stm32g4xx.o" \
    "$G4/boot_obj"/*.o "$G4/hal_obj"/*.o -o "$G4/boot_obj/$G4_BOOT_NAME.elf"; then
    fail "Robot Controller Board bootloader: link failed - see errors above"
    echo ""; echo "$PASS passed, $WARN warnings, $FAIL failed"; exit 1
fi
build_bin_hex "$G4/boot_obj/$G4_BOOT_NAME.elf"
cp "$G4/boot_obj/$G4_BOOT_NAME."{elf,bin,hex} "$FIRMWARE_OUT/"
pass "$G4_BOOT_NAME.bin/.hex/.elf built ($(arm-none-eabi-size "$G4/boot_obj/$G4_BOOT_NAME.elf" | tail -1 | awk '{print $1}') bytes text)"

# -----------------------------------------------------------------------
step "6. Robot Controller Board application (src/mcu_stm32g474/) - FreeRTOS"
# -----------------------------------------------------------------------
SRC="$ROOT/src/mcu_stm32g474"
bump_version "$SRC/boot/bootloader_common.h" FIRMWARE_VERSION "Robot Controller Board application"
if [ "$HYDRA_UMC_CI_MODE" != "1" ]; then
    python3 "$ROOT/bump_manifest_version.py" --sync || exit 1
    # HYDRA_UMC_SCRIPT_STANDARD_VERSION_CAPTURE_AFTER
    HYDRA_UMC_VERSION_AFTER="$(python3 -c 'import json, pathlib, sys; print(json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))["version"])' "$ROOT/hydra-umc.project.json")"
    printf '\n*******************************************************************************\n'
    printf '%s\n' '* VERSION INCREMENT COMPLETED'
    printf '%s\n' "* v${HYDRA_UMC_VERSION_BEFORE:-unknown} -> v${HYDRA_UMC_VERSION_AFTER:-unknown}"
    printf '%s\n' '* Project manifest synchronized with the G474 application version.'
    printf '%s\n' '*******************************************************************************'
    printf '\n'
fi
rm -f "$G4/app_obj"/*.o
arm-none-eabi-gcc $CFLAGS_G4_APP -I"$SRC" -x c -c "$SRC/STM32G474RE_main.c" -o "$G4/app_obj/STM32G474RE_main.o"
# RobotControllerRelay.c: real AXIS_STATUS responder + Tier 2/3 relay tunnel
# app-side logic (docs/architecture.md section 6's own "not yet written" gap,
# now filled) - needs boot/bootloader_common.h for the shared
# CAN_ID_STACKA_BASE/OFS_* constants (see that file's own header on why this
# application re-implements ReadSlotBaseId()/reuses the offsets rather than
# linking the bootloader's own compiled object), so it's the one G474 app
# source that also needs $SRC/boot on its include path.
arm-none-eabi-gcc $CFLAGS_G4_APP -I"$SRC" -I"$SRC/boot" -x c -c "$SRC/RobotControllerRelay.c" -o "$G4/app_obj/RobotControllerRelay.o"
G4_APP_VER="v$(get_version_macro "$SRC/boot/bootloader_common.h" FIRMWARE_VERSION_MAJOR).$(get_version_macro "$SRC/boot/bootloader_common.h" FIRMWARE_VERSION_MINOR).$(get_version_macro "$SRC/boot/bootloader_common.h" FIRMWARE_VERSION_PATCH)"
G4_APP_NAME="HYDRA_RCB_APP_${G4_APP_VER}"
if ! link_filtered arm-none-eabi-gcc $LDCOMMON_G4 -T"$SRC/STM32G474RETx_APP.ld" \
    "$G4/app/startup.o" "$G4/app/system_stm32g4xx.o" \
    "$G4/app_obj"/*.o "$G4/hal_obj"/*.o "$G4/freertos_obj"/*.o -o "$G4/app_obj/$G4_APP_NAME.elf"; then
    fail "Robot Controller Board application: link failed - see errors above"
    echo ""; echo "$PASS passed, $WARN warnings, $FAIL failed"; exit 1
fi
build_bin_hex "$G4/app_obj/$G4_APP_NAME.elf"
cp "$G4/app_obj/$G4_APP_NAME."{elf,bin,hex} "$FIRMWARE_OUT/"
pass "$G4_APP_NAME.bin/.hex/.elf built ($(arm-none-eabi-size "$G4/app_obj/$G4_APP_NAME.elf" | tail -1 | awk '{print $1}') bytes text) - real AXIS_STATUS responder + Tier 2/3 relay tunnel (RobotControllerRelay.c), still HSI clock/no real motion tasks"

fi

# =========================================================================
if [ "$TARGET" = "all" ] || [ "$TARGET" = "h745" ]; then
# =========================================================================
step "7. Kinematic Brain (STM32H745ZIT6) - ST HAL/CMSIS sources"
# -----------------------------------------------------------------------
H7="$BUILD/h745"
mkdir -p "$H7/vendor" "$H7/common/HAL_Include" "$H7/common/CMSIS_Include" "$H7/hal_src" "$H7/hal_obj" "$H7/cm7" "$H7/cm4" "$H7/cm7_boot_obj" "$H7/cm7_app_obj" "$H7/cm4_boot_obj" "$H7/cm4_app_obj" "$H7/cm7_freertos_obj" "$H7/cm4_freertos_obj"

if [ ! -d "$H7/vendor/hal" ]; then
    echo "Fetching STM32H7xx HAL driver ($H7_HAL_TAG)..."
    git -c safe.directory='*' clone --depth 1 --branch "$H7_HAL_TAG" -q "$H7_HAL_REPO" "$H7/vendor/hal"
    pass "HAL driver fetched"
else
    pass "HAL driver already cached at build/h745/vendor/hal"
fi

if [ ! -d "$H7/vendor/cmsis_device_h7" ]; then
    echo "Fetching CMSIS device headers for H7 ($H7_CMSIS_DEVICE_TAG)..."
    git -c safe.directory='*' clone --depth 1 --branch "$H7_CMSIS_DEVICE_TAG" -q "$H7_CMSIS_DEVICE_REPO" "$H7/vendor/cmsis_device_h7"
    pass "CMSIS device (H7) fetched"
else
    pass "CMSIS device (H7) already cached at build/h745/vendor/cmsis_device_h7"
fi

if [ ! -d "$H7/vendor/cmsis_core" ]; then
    echo "Fetching generic ARM CMSIS Core headers (Include/ only, sparse)..."
    git -c safe.directory='*' clone --depth 1 --filter=blob:none --no-checkout -q --branch "$CMSIS_CORE_TAG" "$CMSIS_CORE_REPO" "$H7/vendor/cmsis_core"
    git -C "$H7/vendor/cmsis_core" -c safe.directory='*' sparse-checkout init --no-cone
    echo "/Core/Include/**" > "$H7/vendor/cmsis_core/.git/info/sparse-checkout"
    git -C "$H7/vendor/cmsis_core" -c safe.directory='*' checkout -q
    pass "CMSIS core fetched (Include/ only)"
else
    pass "CMSIS core already cached at build/h745/vendor/cmsis_core"
fi

cp "$H7/vendor/hal/Inc/"*.h "$H7/common/HAL_Include/" 2>/dev/null || true
cp -r "$H7/vendor/hal/Inc/Legacy" "$H7/common/HAL_Include/" 2>/dev/null || true
cp "$H7/common/HAL_Include/stm32h7xx_hal_conf_template.h" "$H7/common/HAL_Include/stm32h7xx_hal_conf.h"
cp "$H7/vendor/hal/Src/"*.c "$H7/hal_src/"
cp "$H7/vendor/cmsis_device_h7/Include/"*.h "$H7/common/CMSIS_Include/"
cp -r "$H7/vendor/cmsis_core/Core/Include/"* "$H7/common/CMSIS_Include/"
if [ -f "$H7/common/CMSIS_Include/core_cm7.h" ] && [ -f "$H7/common/HAL_Include/stm32h7xx_hal.h" ]; then
    pass "HAL/CMSIS include tree assembled"
else
    fail "HAL/CMSIS include tree incomplete - check build/h745/common/ manually"
fi

# -----------------------------------------------------------------------
step "8. Kinematic Brain - common compiler flags and shared HAL objects (per core)"
# -----------------------------------------------------------------------
# App smoke-test core modules plus FDCAN + IWDG + SPI + HSEM - the real
# CAN-OTA bootloaders (src/mcu_stm32h745/CM7/boot/, CM4/boot/): CM7's
# talks FDCAN/IWDG/HSEM only (no bus of its own, see that bootloader's own
# header comment); CM4's is the gateway and needs all four (SPI1 to the
# CM5, FDCAN1 to STACK A, IWDG, HSEM for the CM7<->CM4 mailbox). Compiled
# for both cores regardless (same shared HAL_MODULES_H7 list, see the loop
# below) - unused object files for one core cost nothing, and keeping one
# list avoids the two cores' object sets silently drifting apart.
HAL_MODULES_H7="stm32h7xx_hal stm32h7xx_hal_cortex stm32h7xx_hal_gpio stm32h7xx_hal_rcc stm32h7xx_hal_rcc_ex stm32h7xx_hal_pwr stm32h7xx_hal_pwr_ex stm32h7xx_hal_flash stm32h7xx_hal_flash_ex stm32h7xx_hal_exti stm32h7xx_hal_mdma stm32h7xx_hal_fdcan stm32h7xx_hal_iwdg stm32h7xx_hal_spi stm32h7xx_hal_hsem"

for CORE in CM7 CM4; do
    CFLAGS_H7="-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard -DSTM32H745xx -DCORE_CM4 -I$H7/common/CMSIS_Include -I$H7/common/HAL_Include -O2 -Wall -ffunction-sections -fdata-sections"
    if [ "$CORE" = "CM7" ]; then
        CFLAGS_H7="-mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard -DSTM32H745xx -DCORE_CM7 -I$H7/common/CMSIS_Include -I$H7/common/HAL_Include -O2 -Wall -ffunction-sections -fdata-sections"
    fi
    LOWER=$(echo "$CORE" | tr 'A-Z' 'a-z')
    OBJDIR="$H7/hal_obj_$LOWER"
    mkdir -p "$OBJDIR"
    HAL_OK=1
    for f in $HAL_MODULES_H7; do
        if [ ! -f "$OBJDIR/$f.o" ] || [ "$H7/hal_src/$f.c" -nt "$OBJDIR/$f.o" ]; then
            arm-none-eabi-gcc $CFLAGS_H7 -x c -c "$H7/hal_src/$f.c" -o "$OBJDIR/$f.o" || HAL_OK=0
        fi
    done
    COUNT=$(ls "$OBJDIR"/*.o 2>/dev/null | wc -l)
    if [ "$HAL_OK" = "1" ] && [ "$COUNT" -ge 1 ]; then
        pass "$CORE: $COUNT HAL modules compiled"
    else
        fail "$CORE: one or more HAL modules failed to compile - see errors above"
        echo ""; echo "$PASS passed, $WARN warnings, $FAIL failed"; exit 1
    fi

    CORE_DIR="$H7/$LOWER"
    cp "$H7/vendor/cmsis_device_h7/Source/Templates/gcc/startup_stm32h745xx.s" "$CORE_DIR/"
    cp "$H7/vendor/cmsis_device_h7/Source/Templates/system_stm32h7xx_dualcore_boot_cm4_cm7.c" "$CORE_DIR/system_stm32h7xx.c"
    arm-none-eabi-gcc $CFLAGS_H7 -x assembler-with-cpp -c "$CORE_DIR/startup_stm32h745xx.s" -o "$CORE_DIR/startup.o"
    arm-none-eabi-gcc $CFLAGS_H7 -x c -c "$CORE_DIR/system_stm32h7xx.c" -o "$CORE_DIR/system_stm32h7xx.o"
    pass "$CORE: startup + system files compiled"
done

CFLAGS_CM7="-mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard -DSTM32H745xx -DCORE_CM7 -I$H7/common/CMSIS_Include -I$H7/common/HAL_Include -O2 -Wall -ffunction-sections -fdata-sections"
LDCOMMON_CM7="-mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard -specs=nano.specs -specs=nosys.specs -Wl,--gc-sections"
CFLAGS_CM4="-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard -DSTM32H745xx -DCORE_CM4 -I$H7/common/CMSIS_Include -I$H7/common/HAL_Include -O2 -Wall -ffunction-sections -fdata-sections"
LDCOMMON_CM4="-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard -specs=nano.specs -specs=nosys.specs -Wl,--gc-sections"
# App CFLAGS additionally need each core's own FreeRTOSConfig.h directory
# plus FreeRTOS's own headers/port headers - each core's bootloader
# (bare-metal) uses CFLAGS_CM7/CFLAGS_CM4 alone.
CFLAGS_CM7_APP="$CFLAGS_CM7 -I$ROOT/src/mcu_stm32h745/CM7 -I$FREERTOS_SRC/include -I$FREERTOS_SRC/portable/GCC/ARM_CM7/r0p1"
CFLAGS_CM4_APP="$CFLAGS_CM4 -I$ROOT/src/mcu_stm32h745/CM4 -I$FREERTOS_SRC/include -I$FREERTOS_SRC/portable/GCC/ARM_CM4F"

if compile_freertos "$CFLAGS_CM7_APP" "$FREERTOS_SRC/portable/GCC/ARM_CM7/r0p1" "$H7/cm7_freertos_obj"; then
    pass "FreeRTOS kernel (ARM_CM7/r0p1 port) compiled for CM7"
else
    fail "FreeRTOS kernel failed to compile for CM7 - see errors above"
    echo ""; echo "$PASS passed, $WARN warnings, $FAIL failed"; exit 1
fi
if compile_freertos "$CFLAGS_CM4_APP" "$FREERTOS_SRC/portable/GCC/ARM_CM4F" "$H7/cm4_freertos_obj"; then
    pass "FreeRTOS kernel (ARM_CM4F port) compiled for CM4"
else
    fail "FreeRTOS kernel failed to compile for CM4 - see errors above"
    echo ""; echo "$PASS passed, $WARN warnings, $FAIL failed"; exit 1
fi

# -----------------------------------------------------------------------
step "9. Kinematic Brain CM7 bootloader (bare-metal CAN-OTA, mailbox-relayed) + application (FreeRTOS) - src/mcu_stm32h745/CM7/"
# -----------------------------------------------------------------------
SRC="$ROOT/src/mcu_stm32h745/CM7"
COMMON_INC="-I$ROOT/src/mcu_stm32h745/Common"
bump_version "$SRC/boot/bootloader_common.h" BOOTLOADER_VERSION "Kinematic Brain CM7 bootloader"
if compile_dir "$SRC/boot" "$H7/cm7_boot_obj" "$CFLAGS_CM7" "$COMMON_INC"; then
    pass "CM7 bootloader_*.c compiled ($(ls "$SRC/boot"/*.c | wc -l) files)"
else
    fail "CM7 bootloader failed to compile - see errors above"
    echo ""; echo "$PASS passed, $WARN warnings, $FAIL failed"; exit 1
fi
CM7_BOOT_VER="v$(get_version_macro "$SRC/boot/bootloader_common.h" BOOTLOADER_VERSION_MAJOR).$(get_version_macro "$SRC/boot/bootloader_common.h" BOOTLOADER_VERSION_MINOR).$(get_version_macro "$SRC/boot/bootloader_common.h" BOOTLOADER_VERSION_PATCH)"
CM7_BOOT_NAME="HYDRA_KB_CM7_BOOTLOADER_${CM7_BOOT_VER}"
if ! link_filtered arm-none-eabi-gcc $LDCOMMON_CM7 -T"$SRC/boot/STM32H745ZITx_CM7_BOOTLOADER.ld" \
    "$H7/cm7/startup.o" "$H7/cm7/system_stm32h7xx.o" \
    "$H7/cm7_boot_obj"/*.o "$H7/hal_obj_cm7"/*.o -o "$H7/cm7_boot_obj/$CM7_BOOT_NAME.elf"; then
    fail "CM7 bootloader: link failed - see errors above"
    echo ""; echo "$PASS passed, $WARN warnings, $FAIL failed"; exit 1
fi
build_bin_hex "$H7/cm7_boot_obj/$CM7_BOOT_NAME.elf"
cp "$H7/cm7_boot_obj/$CM7_BOOT_NAME."{elf,bin,hex} "$FIRMWARE_OUT/"
pass "$CM7_BOOT_NAME.bin/.hex/.elf built ($(arm-none-eabi-size "$H7/cm7_boot_obj/$CM7_BOOT_NAME.elf" | tail -1 | awk '{print $1}') bytes text)"

bump_version "$SRC/boot/bootloader_common.h" FIRMWARE_VERSION "Kinematic Brain CM7 application"
arm-none-eabi-gcc $CFLAGS_CM7_APP -I"$SRC" -x c -c "$SRC/STM32H745ZI_CM7_main.c" -o "$H7/cm7_app_obj/STM32H745ZI_CM7_main.o"
CM7_APP_VER="v$(get_version_macro "$SRC/boot/bootloader_common.h" FIRMWARE_VERSION_MAJOR).$(get_version_macro "$SRC/boot/bootloader_common.h" FIRMWARE_VERSION_MINOR).$(get_version_macro "$SRC/boot/bootloader_common.h" FIRMWARE_VERSION_PATCH)"
CM7_APP_NAME="HYDRA_KB_CM7_APP_${CM7_APP_VER}"
if ! link_filtered arm-none-eabi-gcc $LDCOMMON_CM7 -T"$SRC/STM32H745ZITx_CM7_APP.ld" \
    "$H7/cm7/startup.o" "$H7/cm7/system_stm32h7xx.o" \
    "$H7/cm7_app_obj"/*.o "$H7/hal_obj_cm7"/*.o "$H7/cm7_freertos_obj"/*.o -o "$H7/cm7_app_obj/$CM7_APP_NAME.elf"; then
    fail "CM7 application: link failed - see errors above"
    echo ""; echo "$PASS passed, $WARN warnings, $FAIL failed"; exit 1
fi
build_bin_hex "$H7/cm7_app_obj/$CM7_APP_NAME.elf"
cp "$H7/cm7_app_obj/$CM7_APP_NAME."{elf,bin,hex} "$FIRMWARE_OUT/"
pass "$CM7_APP_NAME.bin/.hex/.elf built ($(arm-none-eabi-size "$H7/cm7_app_obj/$CM7_APP_NAME.elf" | tail -1 | awk '{print $1}') bytes text) - FreeRTOS GPIO-toggle smoke test"

# -----------------------------------------------------------------------
step "10. Kinematic Brain CM4 bootloader (bare-metal CAN-OTA gateway: SPI1+FDCAN1+mailbox) + application (FreeRTOS) - src/mcu_stm32h745/CM4/"
# -----------------------------------------------------------------------
SRC="$ROOT/src/mcu_stm32h745/CM4"
bump_version "$SRC/boot/bootloader_common.h" BOOTLOADER_VERSION "Kinematic Brain CM4 bootloader"
if compile_dir "$SRC/boot" "$H7/cm4_boot_obj" "$CFLAGS_CM4" "$COMMON_INC"; then
    pass "CM4 bootloader_*.c compiled ($(ls "$SRC/boot"/*.c | wc -l) files)"
else
    fail "CM4 bootloader failed to compile - see errors above"
    echo ""; echo "$PASS passed, $WARN warnings, $FAIL failed"; exit 1
fi
CM4_BOOT_VER="v$(get_version_macro "$SRC/boot/bootloader_common.h" BOOTLOADER_VERSION_MAJOR).$(get_version_macro "$SRC/boot/bootloader_common.h" BOOTLOADER_VERSION_MINOR).$(get_version_macro "$SRC/boot/bootloader_common.h" BOOTLOADER_VERSION_PATCH)"
CM4_BOOT_NAME="HYDRA_KB_CM4_BOOTLOADER_${CM4_BOOT_VER}"
if ! link_filtered arm-none-eabi-gcc $LDCOMMON_CM4 -T"$SRC/boot/STM32H745ZITx_CM4_BOOTLOADER.ld" \
    "$H7/cm4/startup.o" "$H7/cm4/system_stm32h7xx.o" \
    "$H7/cm4_boot_obj"/*.o "$H7/hal_obj_cm4"/*.o -o "$H7/cm4_boot_obj/$CM4_BOOT_NAME.elf"; then
    fail "CM4 bootloader: link failed - see errors above"
    echo ""; echo "$PASS passed, $WARN warnings, $FAIL failed"; exit 1
fi
build_bin_hex "$H7/cm4_boot_obj/$CM4_BOOT_NAME.elf"
cp "$H7/cm4_boot_obj/$CM4_BOOT_NAME."{elf,bin,hex} "$FIRMWARE_OUT/"
pass "$CM4_BOOT_NAME.bin/.hex/.elf built ($(arm-none-eabi-size "$H7/cm4_boot_obj/$CM4_BOOT_NAME.elf" | tail -1 | awk '{print $1}') bytes text)"

bump_version "$SRC/boot/bootloader_common.h" FIRMWARE_VERSION "Kinematic Brain CM4 application"
arm-none-eabi-gcc $CFLAGS_CM4_APP -I"$SRC" -x c -c "$SRC/STM32H745ZI_CM4_main.c" -o "$H7/cm4_app_obj/STM32H745ZI_CM4_main.o"
# KinematicBrainCan.c: real FDCAN1 "STACK A" master + Tier 2/3 relay tunnel -
# needs bootloader_common.h for the shared CAN_ID_STACKA_BASE/OFS_* constants
# (see that file's own header on why this application reuses it rather than
# duplicating it), so it's the one CM4 app source that also needs $SRC/boot
# on its include path.
arm-none-eabi-gcc $CFLAGS_CM4_APP -I"$SRC" -I"$SRC/boot" -x c -c "$SRC/KinematicBrainCan.c" -o "$H7/cm4_app_obj/KinematicBrainCan.o"
CM4_APP_VER="v$(get_version_macro "$SRC/boot/bootloader_common.h" FIRMWARE_VERSION_MAJOR).$(get_version_macro "$SRC/boot/bootloader_common.h" FIRMWARE_VERSION_MINOR).$(get_version_macro "$SRC/boot/bootloader_common.h" FIRMWARE_VERSION_PATCH)"
CM4_APP_NAME="HYDRA_KB_CM4_APP_${CM4_APP_VER}"
if ! link_filtered arm-none-eabi-gcc $LDCOMMON_CM4 -T"$SRC/STM32H745ZITx_CM4_APP.ld" \
    "$H7/cm4/startup.o" "$H7/cm4/system_stm32h7xx.o" \
    "$H7/cm4_app_obj"/*.o "$H7/hal_obj_cm4"/*.o "$H7/cm4_freertos_obj"/*.o -o "$H7/cm4_app_obj/$CM4_APP_NAME.elf"; then
    fail "CM4 application: link failed - see errors above"
    echo ""; echo "$PASS passed, $WARN warnings, $FAIL failed"; exit 1
fi
build_bin_hex "$H7/cm4_app_obj/$CM4_APP_NAME.elf"
cp "$H7/cm4_app_obj/$CM4_APP_NAME."{elf,bin,hex} "$FIRMWARE_OUT/"
pass "$CM4_APP_NAME.bin/.hex/.elf built ($(arm-none-eabi-size "$H7/cm4_app_obj/$CM4_APP_NAME.elf" | tail -1 | awk '{print $1}') bytes text) - real PLL1 clock config + real FDCAN1 STACK A + Tier 2/3 relay tunnel, still no CM7<->CM4 scheduler sync"

fi

# -----------------------------------------------------------------------
step "11. Firmware manifest (firmware/firmware_manifest.json)"
# -----------------------------------------------------------------------
# Only meaningful once every component exists - a partial build (g474-only
# or h745-only) would make generate_manifest.py fail looking for the other
# chip's own binaries, so this only runs for a full 'all' build (the
# default with no target argument, or an explicit 'all').
if [ "$TARGET" = "all" ] && [ "$HYDRA_UMC_CI_MODE" != "1" ]; then
    # python3 is a hard requirement (step 1), so no availability check needed here.
    if python3 "$ROOT/generate_manifest.py" "$ROOT"; then
        pass "firmware_manifest.json regenerated - see it for exact versions/CRC32 of every component just built"
    else
        fail "generate_manifest.py failed - see errors above"
    fi
else
    echo "  (skipped - CI never rewrites the manifest; partial builds do not regenerate it either)"
fi

# -----------------------------------------------------------------------
step "Summary"
# -----------------------------------------------------------------------
echo "$PASS passed, $WARN warnings, $FAIL failed"
echo ""
echo "Output binaries are in: $FIRMWARE_OUT/"
echo ""
echo "Reminder: the CM4 and G474 _APP binaries now carry real FDCAN Tier"
echo "0/1 traffic (STACK A master + AXIS_STATUS responder) and the real"
echo "Tier 2/3 RELAY_SEND/RELAY_RECV tunnel end to end - CM7's own _APP"
echo "and every board's real motion-control code (step/dir/en pulse"
echo "generation, S-curve profiles) are still the FreeRTOS GPIO-toggle"
echo "smoke test, not yet written. Every _BOOTLOADER binary IS the real"
echo "CAN-OTA/SPI-OTA protocol (bare-metal, no FreeRTOS by design) - none"
echo "of this is yet verified against real hardware. See each"
echo "src/*/README.md and docs/architecture.md."

if [ "$FAIL" -gt 0 ]; then exit 1; fi
